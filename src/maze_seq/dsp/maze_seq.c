/* =============================================================================
 * MAZE  -  Overtake (tool) DSP for Ableton Move (Schwung)     [id: maze_seq]
 * -----------------------------------------------------------------------------
 * Dual 8-step generative sequencer (Moog Labyrinth style), clock-synced to the
 * Move transport, MIDI out per sequencer, with disk persistence.
 * Mirrors the stock tb3po.c tool DSP (plugin_api_v2, move_plugin_init_v2).
 *
 * REALTIME SAFETY (important):
 *   set_param / get_param / create_instance / on_midi / render_block all run on
 *   the SPI AUDIO CALLBACK. File I/O there causes device-wide audio dropouts.
 *   So state SAVES happen on a background SCHED_OTHER worker thread: the audio
 *   thread only sets a dirty flag (set_param "save"), and the worker does the
 *   fopen/fwrite. Load happens once in create_instance (one-time, like tb3po).
 *
 * TRANSPOSE MODEL:
 *   - "key" (0..11) = pitch class, owned by the UI (Key knob).
 *   - "pad_semis"   = semitone transpose from the pad keyboard.
 *   - root = 60 + key + transpose(oct buttons) + pad_semis.
 *   Incoming notes do NOT set key (that fought the knob); the UI owns it.
 *
 * RANGES: corrupt and cv_range are 0..100.
 * Persistence: /data/UserData/schwung/tool_state/maze_seq.bin (version 3).
 *
 * You (a non-coder) only ever need to touch bits marked  ==>> EDIT ME.
 * ===========================================================================*/

#include "plugin_api_v1.h"   /* compiled with -Isrc/include */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#define NUM_STEPS   8

#define MAZE_STATE_DIR  "/data/UserData/schwung/tool_state"
#define MAZE_STATE_PATH MAZE_STATE_DIR "/maze_seq.bin"
#define MAZE_STATE_MAGIC 0x455A414Du   /* "MAZE" */
#define MAZE_STATE_VERSION 3u

static uint32_t rng_state = 0x1234abcdu;
static inline uint32_t rng_next(void){ uint32_t x=rng_state; x^=x<<13; x^=x>>17; x^=x<<5; rng_state=x; return x; }
static inline float rng_f(void){ return (rng_next()>>8)*(1.0f/16777216.0f); }
static inline float rng_bip(void){ return rng_f()*2.0f-1.0f; }

/* ---- Scales  (==>> EDIT ME: keep in sync with SCALES list in ui.js) ---- */
typedef struct { const int8_t *pc; int n; } scale_t;
static const int8_t SC_CHROM[]={0,1,2,3,4,5,6,7,8,9,10,11};
static const int8_t SC_MAJ[]  ={0,2,4,5,7,9,11};
static const int8_t SC_MIN[]  ={0,2,3,5,7,8,10};
static const int8_t SC_PMAJ[] ={0,2,4,7,9};
static const int8_t SC_PMIN[] ={0,3,5,7,10};
static const int8_t SC_MEL[]  ={0,2,3,5,7,9,11};
static const int8_t SC_HARM[] ={0,2,3,5,7,8,11};
static const int8_t SC_WHOLE[]={0,2,4,6,8,10};
static const int8_t SC_HIRA[] ={0,2,3,7,8};
static const int8_t SC_MAJ7[] ={0,4,7,11};
static const int8_t SC_MIN7[] ={0,3,7,10};
static const int8_t SC_UNQ[]  ={0,1,2,3,4,5,6,7,8,9,10,11};
static const scale_t SCALES[]={
    {SC_CHROM,12},{SC_MAJ,7},{SC_MIN,7},{SC_PMAJ,5},{SC_PMIN,5},
    {SC_MEL,7},{SC_HARM,7},{SC_WHOLE,6},{SC_HIRA,5},{SC_MAJ7,4},{SC_MIN7,4},{SC_UNQ,12}
};
#define NUM_SCALES ((int)(sizeof(SCALES)/sizeof(SCALES[0])))

static const int RATE_PULSES[]={3,6,12,24,48,96};
#define NUM_RATES ((int)(sizeof(RATE_PULSES)/sizeof(RATE_PULSES[0])))
static const float GATE_STEPS[]={0.25f,0.5f,0.75f,1.0f,1.25f,1.5f,1.75f,2.0f};
#define NUM_GATES ((int)(sizeof(GATE_STEPS)/sizeof(GATE_STEPS[0])))

/* Sequence Reset (Reset Both knob): snap both play heads to step 1 every N BARS.
   UI sends index 0..4 -> 1,2,4,8 bars, 0 = off. One bar = 96 clock pulses;
   every note rate divides it evenly, so N bars = N*(96/RATE_PULSES) steps.
   Keep in sync with RESET_LABELS in ui.js. */
#define PULSES_PER_BAR 96
static const int RESET_BARS[]={1,2,4,8,0};
#define NUM_RESETS ((int)(sizeof(RESET_BARS)/sizeof(RESET_BARS[0])))
static inline int reset_bars_from_idx(int idx){
    if (idx<0) idx=0;
    if (idx>=NUM_RESETS) idx=NUM_RESETS-1;
    return RESET_BARS[idx];
}

#define MAX_SPREAD 64   /* ==>> EDIT ME: semitone spread each side of root */

typedef struct {
    int   bit[NUM_STEPS];
    float cv [NUM_STEPS];
    int   length, play, corrupt, cv_range;   /* corrupt/cv_range: 0..100 */
    int   channel;
    int   reset_bars, reset_ctr;             /* reset_bars: bars per reset, 0 = off; reset_ctr counts steps */
    int   last_note, last_ch; long off_pulse; int note_active;
} seq_t;

typedef struct {
    const host_api_v1_t *host;
    seq_t s[2];
    int   scale, key, rate, gate, trig_mix;
    int   root, transpose;
    int   pad_semis;
    int   running, suspended;
    long  pulse;

    /* background state saver */
    pthread_t       state_thread;
    int             state_thread_started;
    volatile int    state_thread_stop;
    volatile int    state_dirty;
    pthread_mutex_t state_mutex;
} maze_t;

static const host_api_v1_t *g_host = NULL;

static void seq_randomize(seq_t *q){
    q->length=NUM_STEPS; q->play=-1;
    for (int i=0;i<NUM_STEPS;i++){ q->bit[i]=(rng_f()<0.5f)?1:0; q->cv[i]=rng_bip(); }
}
static void seq_corrupt(seq_t *q, int idx){
    float c=q->corrupt/100.0f;
    float p_cv =(c<=0.5f)?(c*0.5f):(0.25f+(c-0.5f)*0.5f);
    float p_bit=(c<=0.5f)?0.0f:((c-0.5f));
    if (rng_f()<p_cv) q->cv[idx]=rng_bip();
    if (p_bit>0.0f && rng_f()<p_bit){ q->bit[idx]=!q->bit[idx]; if(q->bit[idx]) q->cv[idx]=rng_bip(); }
}
static int quantize_note(const maze_t *L, float cv, int cv_range){
    float scaled=cv*(cv_range/100.0f);
    int semi=(int)lrintf(scaled*MAX_SPREAD);
    int note=L->root+semi;
    if (note<0) note=0; if (note>127) note=127;
    const scale_t *sc=&SCALES[L->scale];
    int root_pc=((L->root%12)+12)%12;
    int pc=((note%12)+12)%12, best_pc=root_pc, bd=128;
    for (int i=0;i<sc->n;i++){
        int cand=(root_pc+sc->pc[i])%12;
        int d=abs(pc-cand); if(d>6)d=12-d;
        if(d<bd){ bd=d; best_pc=cand; }
    }
    int diff=best_pc-pc; if(diff>6)diff-=12; if(diff<-6)diff+=12;
    note+=diff;
    if (note<0) note+=12; if (note>127) note-=12;
    return note;
}
static void trig_velocities(int trig_mix, int *v1, int *v2){
    if (trig_mix<=0){
        float f=(trig_mix+63)/63.0f;
        *v1=(int)lrintf(127.0f-27.0f*f);
        *v2=(trig_mix==-63)?0:(int)lrintf(1.0f+99.0f*f);
    } else {
        float f=trig_mix/64.0f;
        *v1=(int)lrintf(100.0f*(1.0f-f));
        *v2=(int)lrintf(100.0f+27.0f*f);
    }
    if(*v1<0)*v1=0; if(*v1>127)*v1=127; if(*v2<0)*v2=0; if(*v2>127)*v2=127;
}

static void send_midi(maze_t *L, uint8_t status, uint8_t d1, uint8_t d2){
    if (!L->host || !L->host->midi_send_internal) return;
    uint8_t pkt[4]={ (uint8_t)((status>>4)&0x0F), status, d1, d2 };
    L->host->midi_send_internal(pkt,4);
}
static void seq_note_off(maze_t *L, seq_t *q){
    if (q->note_active){ send_midi(L,0x80|(q->last_ch&0x0F),q->last_note&0x7F,0); q->note_active=0; }
}
static void all_notes_off(maze_t *L){ seq_note_off(L,&L->s[0]); seq_note_off(L,&L->s[1]); }

static void step_seq(maze_t *L, int which, int vel){
    seq_t *q=&L->s[which];
    int n=q->length<1?1:q->length;
    /* Sequence Reset: every reset_bars bars, snap the play head back to step 1.
       steps/bar = 96/RATE_PULSES at the current note rate. */
    if (q->reset_bars>0){
        int thresh = q->reset_bars * (PULSES_PER_BAR / RATE_PULSES[L->rate]);
        if (thresh>0 && q->reset_ctr>=thresh){ q->play=-1; q->reset_ctr=0; }
    }
    q->play=(q->play+1)%n;
    q->reset_ctr++;
    seq_corrupt(q,q->play);
    if (q->bit[q->play] && vel>0){
        seq_note_off(L,q);
        int note=quantize_note(L,q->cv[q->play],q->cv_range);
        send_midi(L,0x90|(q->channel&0x0F),note&0x7F,vel&0x7F);
        q->last_note=note; q->last_ch=q->channel; q->note_active=1;
        q->off_pulse=L->pulse+(long)lrintf(GATE_STEPS[L->gate]*RATE_PULSES[L->rate]);
        if (q->off_pulse<=L->pulse) q->off_pulse=L->pulse+1;
    }
}
static void flush_offs(maze_t *L){
    for (int i=0;i<2;i++){ seq_t *q=&L->s[i]; if(q->note_active && L->pulse>=q->off_pulse) seq_note_off(L,q); }
}
static void transport_reset(maze_t *L){
    L->pulse=0;
    L->s[0].play=-1; L->s[1].play=-1;
    L->s[0].reset_ctr=0; L->s[1].reset_ctr=0;
}
static void set_root_from_key(maze_t *L){
    int r=60+(L->key%12)+L->transpose+L->pad_semis;
    if(r<0)r=0; if(r>127)r=127; L->root=r;
}

/* ---- persistence (runs on the WORKER thread, never the audio callback) ---- */
static int ensure_state_dir(void){
    struct stat st;
    if (stat(MAZE_STATE_DIR,&st)==0 && S_ISDIR(st.st_mode)) return 1;
    if (mkdir(MAZE_STATE_DIR,0755)==0) return 1;
    return (stat(MAZE_STATE_DIR,&st)==0 && S_ISDIR(st.st_mode));
}
static void write_seq(FILE *f, const seq_t *q){
    int32_t i;
    for (int k=0;k<NUM_STEPS;k++){ i=(int32_t)q->bit[k]; fwrite(&i,4,1,f); }
    for (int k=0;k<NUM_STEPS;k++){ float v=q->cv[k]; fwrite(&v,4,1,f); }
    i=q->length;  fwrite(&i,4,1,f);
    i=q->play;    fwrite(&i,4,1,f);
    i=q->corrupt; fwrite(&i,4,1,f);
    i=q->cv_range;fwrite(&i,4,1,f);
    i=q->channel; fwrite(&i,4,1,f);
}
static int read_seq(FILE *f, seq_t *q){
    int32_t i; float v;
    for (int k=0;k<NUM_STEPS;k++){ if(fread(&i,4,1,f)!=1) return 0; q->bit[k]=i?1:0; }
    for (int k=0;k<NUM_STEPS;k++){ if(fread(&v,4,1,f)!=1) return 0; q->cv[k]=v; }
    if(fread(&i,4,1,f)!=1) return 0; q->length  =(i<1?1:(i>8?8:i));
    if(fread(&i,4,1,f)!=1) return 0; q->play    =i;
    if(fread(&i,4,1,f)!=1) return 0; q->corrupt =(i<0?0:(i>100?100:i));
    if(fread(&i,4,1,f)!=1) return 0; q->cv_range=(i<0?0:(i>100?100:i));
    if(fread(&i,4,1,f)!=1) return 0; q->channel =(i<0?0:(i>15?15:i));
    return 1;
}
/* Do the actual file write. Guarded by state_mutex so the worker and the
   final destroy-time save can't race on the same FILE*. */
static void maze_save_state_locked(maze_t *L){
    if (!L || !ensure_state_dir()) return;
    FILE *f=fopen(MAZE_STATE_PATH,"wb");
    if(!f) return;
    uint32_t u; int32_t i;
    u=MAZE_STATE_MAGIC;   fwrite(&u,4,1,f);
    u=MAZE_STATE_VERSION; fwrite(&u,4,1,f);
    i=L->scale;    fwrite(&i,4,1,f);
    i=L->key;      fwrite(&i,4,1,f);
    i=L->rate;     fwrite(&i,4,1,f);
    i=L->gate;     fwrite(&i,4,1,f);
    i=L->trig_mix; fwrite(&i,4,1,f);
    i=L->transpose;fwrite(&i,4,1,f);
    i=L->pad_semis;fwrite(&i,4,1,f);
    i=L->root;     fwrite(&i,4,1,f);
    write_seq(f,&L->s[0]);
    write_seq(f,&L->s[1]);
    fclose(f);
}
static void maze_save_state(maze_t *L){
    if(!L) return;
    pthread_mutex_lock(&L->state_mutex);
    maze_save_state_locked(L);
    pthread_mutex_unlock(&L->state_mutex);
}
static int maze_load_state(maze_t *L){
    FILE *f=fopen(MAZE_STATE_PATH,"rb");
    if(!f) return 0;
    uint32_t magic=0,ver=0; int32_t i;
    if(fread(&magic,4,1,f)!=1||fread(&ver,4,1,f)!=1||magic!=MAZE_STATE_MAGIC||ver!=MAZE_STATE_VERSION){ fclose(f); return 0; }
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->scale=(i<0?0:(i>=NUM_SCALES?NUM_SCALES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->key=((i%12)+12)%12;
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->rate=(i<0?0:(i>=NUM_RATES?NUM_RATES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->gate=(i<0?0:(i>=NUM_GATES?NUM_GATES-1:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->trig_mix=(i<-63?-63:(i>64?64:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->transpose=(i<-48?-48:(i>48?48:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->pad_semis=(i<-60?-60:(i>60?60:i));
    if(fread(&i,4,1,f)!=1){fclose(f);return 0;} L->root=(i<0?0:(i>127?127:i));
    if(!read_seq(f,&L->s[0])){fclose(f);return 0;}
    if(!read_seq(f,&L->s[1])){fclose(f);return 0;}
    fclose(f);
    return 1;
}

/* Background worker: wake every ~2s, flush if dirty. Demotes itself off the
   audio priority it inherits, and keeps core 3 free for SPI. */
static void *maze_state_worker(void *arg){
    maze_t *L=(maze_t*)arg;
    struct sched_param sp; sp.sched_priority=0;
    sched_setscheduler(0, SCHED_OTHER, &sp);     /* MUST be first */
#ifdef CPU_ZERO
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0,&set); CPU_SET(1,&set); CPU_SET(2,&set);   /* not core 3 */
    sched_setaffinity(0, sizeof(set), &set);
#endif
    while(!L->state_thread_stop){
        for(int i=0;i<10 && !L->state_thread_stop;i++) usleep(200*1000); /* ~2s */
        if(L->state_thread_stop) break;
        if(L->state_dirty){ maze_save_state(L); L->state_dirty=0; }
    }
    return NULL;
}

/* =============================================================================
 *  Plugin ABI (plugin_api_v2)
 * ===========================================================================*/
static void *maze_create(const char *module_dir, const char *json_defaults){
    (void)module_dir;(void)json_defaults;
    maze_t *L=(maze_t*)calloc(1,sizeof(maze_t));
    if(!L) return NULL;
    L->host=g_host;
    rng_state^=(uint32_t)(uintptr_t)L|0x9e3779b9u;
    L->scale=1; L->key=0; L->rate=1; L->gate=3; L->trig_mix=0;
    L->transpose=0; L->root=60;
    seq_randomize(&L->s[0]); seq_randomize(&L->s[1]);
    L->s[0].cv_range=20; L->s[0].channel=0;   /* ==>> EDIT ME: default Seq1 ch */
    L->s[1].cv_range=20; L->s[1].channel=0;   /* ==>> EDIT ME: default Seq2 ch */
    L->s[0].reset_bars=0; L->s[1].reset_bars=0; /* default: off (never reset) */

    pthread_mutex_init(&L->state_mutex, NULL);
    maze_load_state(L);                        /* one-time load (like tb3po) */

    L->running=0; L->suspended=0; L->pulse=0;

    /* start the background saver */
    L->state_thread_stop=0; L->state_dirty=0;
    if (pthread_create(&L->state_thread, NULL, maze_state_worker, L)==0)
        L->state_thread_started=1;
    return L;
}
static void maze_destroy(void *inst){
    maze_t *L=(maze_t*)inst;
    if(!L) return;
    all_notes_off(L);
    /* stop the worker, then one final synchronous save */
    L->state_thread_stop=1;
    if(L->state_thread_started) pthread_join(L->state_thread, NULL);
    maze_save_state(L);
    pthread_mutex_destroy(&L->state_mutex);
    free(L);
}

static void maze_on_midi(void *inst, const uint8_t *msg, int len, int source){
    (void)source;
    maze_t *L=(maze_t*)inst;
    if(!L||!msg||len<1) return;
    uint8_t st=msg[0];
    if (st==0xFA){ transport_reset(L); L->running=1; return; }
    if (st==0xFB){ L->running=1; return; }
    if (st==0xFC){ L->running=0; all_notes_off(L); return; }
    if (st==0xF8){
        if(!L->running) return;
        L->pulse++;
        flush_offs(L);
        if (L->pulse % RATE_PULSES[L->rate] == 0){
            int v1,v2; trig_velocities(L->trig_mix,&v1,&v2);
            step_seq(L,0,v1); step_seq(L,1,v2);
        }
        return;
    }
    /* incoming notes intentionally do NOT set key (UI owns it). */
}

static void maze_set_param(void *inst, const char *key, const char *val){
    maze_t *L=(maze_t*)inst;
    if(!L||!key||!val) return;
    int v=atoi(val);
    if      (!strcmp(key,"s1_corrupt"))  L->s[0].corrupt=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s1_cv_range")) L->s[0].cv_range=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s1_length"))   L->s[0].length=(v<1?1:(v>8?8:v));
    else if (!strcmp(key,"s1_channel"))  L->s[0].channel=(v<0?0:(v>15?15:v));
    else if (!strcmp(key,"s1_flip")){ int p=(v<0||v>7)?0:v; L->s[0].bit[p]=!L->s[0].bit[p]; if(L->s[0].bit[p])L->s[0].cv[p]=rng_bip(); }
    else if (!strcmp(key,"s1_adv")){ int n=L->s[0].length<1?1:L->s[0].length; L->s[0].play=((L->s[0].play+(v<0?-1:1))%n+n)%n; }
    else if (!strcmp(key,"s1_len_dec")){ L->s[0].length=(L->s[0].length<=1)?8:L->s[0].length-1; }
    else if (!strcmp(key,"s2_corrupt"))  L->s[1].corrupt=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s2_cv_range")) L->s[1].cv_range=(v<0?0:(v>100?100:v));
    else if (!strcmp(key,"s2_length"))   L->s[1].length=(v<1?1:(v>8?8:v));
    else if (!strcmp(key,"s2_channel"))  L->s[1].channel=(v<0?0:(v>15?15:v));
    else if (!strcmp(key,"s2_flip")){ int p=(v<0||v>7)?0:v; L->s[1].bit[p]=!L->s[1].bit[p]; if(L->s[1].bit[p])L->s[1].cv[p]=rng_bip(); }
    else if (!strcmp(key,"s2_adv")){ int n=L->s[1].length<1?1:L->s[1].length; L->s[1].play=((L->s[1].play+(v<0?-1:1))%n+n)%n; }
    else if (!strcmp(key,"s2_len_dec")){ L->s[1].length=(L->s[1].length<=1)?8:L->s[1].length-1; }
    else if (!strcmp(key,"trig_mix"))    L->trig_mix=(v<-63?-63:(v>64?64:v));
    /* Reset Both: one global reset length (in bars) for both sequencers. */
    else if (!strcmp(key,"g_reset")){ int rb=reset_bars_from_idx(v); L->s[0].reset_bars=rb; L->s[1].reset_bars=rb; }
    else if (!strcmp(key,"scale"))       L->scale=(v<0?0:(v>=NUM_SCALES?NUM_SCALES-1:v));
    else if (!strcmp(key,"key")){ L->key=((v%12)+12)%12; set_root_from_key(L); }
    else if (!strcmp(key,"note_rate"))   L->rate=(v<0?0:(v>=NUM_RATES?NUM_RATES-1:v));
    else if (!strcmp(key,"note_length")) L->gate=(v<0?0:(v>=NUM_GATES?NUM_GATES-1:v));
    else if (!strcmp(key,"transpose")){ L->transpose=(v<-48?-48:(v>48?48:v)); set_root_from_key(L); }
    else if (!strcmp(key,"pad_semis")){ L->pad_semis=(v<-60?-60:(v>60?60:v)); set_root_from_key(L); }
    else if (!strcmp(key,"suspend")){ L->suspended=(v!=0); }
    else if (!strcmp(key,"panic")){ all_notes_off(L); }
    /* RT-safe save: just mark dirty; the worker thread does the file I/O. */
    else if (!strcmp(key,"save")){ L->state_dirty=1; }
}

static int maze_get_param(void *inst, const char *key, char *buf, int buf_len){
    maze_t *L=(maze_t*)inst;
    if(!L||!key||!buf||buf_len<2) return -1;
    int n=0;
    if (!strcmp(key,"running")) n=snprintf(buf,buf_len,"%d",L->running?1:0);
    else if (!strcmp(key,"s1_state")||!strcmp(key,"s2_state")){
        seq_t *q=&L->s[key[1]=='2'?1:0];
        int off=snprintf(buf,buf_len,"%d|",q->length);
        for (int i=0;i<NUM_STEPS && off<buf_len-2;i++)
            off+=snprintf(buf+off,buf_len-off,"%s%d", i?",":"", q->bit[i]);
        if (off<buf_len-2) off+=snprintf(buf+off,buf_len-off,"|%d", q->play);
        n=off;
    }
    else return -1;
    if (n<0) return -1; if (n>=buf_len) n=buf_len-1;
    return n;
}
static int maze_get_error(void *inst, char *buf, int buf_len){ (void)inst;(void)buf;(void)buf_len; return 0; }
static void maze_render_block(void *inst, int16_t *out, int frames){
    (void)inst;
    if (out && frames>0) memset(out,0,sizeof(int16_t)*frames*2);
}

static plugin_api_v2_t g_api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = maze_create,
    .destroy_instance = maze_destroy,
    .on_midi          = maze_on_midi,
    .set_param        = maze_set_param,
    .get_param        = maze_get_param,
    .get_error        = maze_get_error,
    .render_block     = maze_render_block,
};
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host){
    g_host = host;
    return &g_api;
}
