/* ============================================================================
 *  maze_voice.c  —  Schwung sound generator, v0.3.6
 *  "Maze Voice" — Moog Labyrinth-style thru-zero oscillator / wavefolder /
 *  state-variable filter voice for Ableton Move.
 *
 *  v0.3.6 change — Randomise page, module.json only, no DSP change:
 *    rnd_voice / rnd_wavefolder / rnd_filter / rnd_tone now declare
 *    access:"readwrite" so the knob grid draws them as LATCHING TOGGLES.
 *    Without it the grid's trigger heuristic sees "rnd_*" + a two-state
 *    enum and infers a momentary button (writeOnly), so the four arming
 *    switches behaved like one-shot actions. rnd_go stays access:"write"
 *    (it really is momentary — press to Generate).
 *
 *  v0.3.5 change — packaging / robustness only, no DSP behaviour change:
 *    - module.json trimmed under the host's 8 KB parser cap (the duplicated
 *      top-level chain_params block is gone; chain_params + ui_hierarchy now
 *      live inside "capabilities" where the chain scanner and the DSP
 *      self-read both find them). v0.3.4's 11.6 KB module.json was silently
 *      rejected by module_manager.c, so Maze Voice never appeared in the menu.
 *    - on_midi() now ignores note numbers 0..9 (Move capacitive knob-touch
 *      notes): with raw_midi:true the host does not filter them, so a knob
 *      touch was retriggering the envelope at sub-audio pitch.
 *
 *  v0.3.4 change — RANDOMISE redesigned as a dedicated "Randomise" page:
 *    four page TOGGLES (Voice / WaveFolder / Filter / Tone) select which pages
 *    get randomised, plus ONE momentary "Generate" button that randomises the
 *    parameters of every toggled-on page at once. Replaces the earlier
 *    shift+jog gesture / per-page button (those are removed). This also drops
 *    the shadow-control SHM code and the -lrt link dependency.
 *      Params: rnd_voice, rnd_wavefolder, rnd_filter, rnd_tone  (enum off/on),
 *              rnd_go  (write-enum off/go, momentary — fires once, never latches).
 *    A set_param("randomize","voice|wavefolder|filter|tone|armed") scripting
 *    fallback is also provided.
 *
 *  v0.3.2 (retained): Filter Cutoff + both decays are 0-100 knobs, exponentially
 *  mapped in the DSP (cutoff 20 Hz..20 kHz; decays 5 ms..2 s). Preset state()
 *  serialise/restore. Everything from v0.3.0/0.3.1 (display-unit knobs,
 *  per-channel warm mixer overdrive, Boss OD on the VCA output, 2 ms envelope
 *  attack ramp, fold/FM smoothing, forge.c self-serve of chain_params/
 *  ui_hierarchy) is verbatim.
 *
 *  API: plugin_api_v2. Audio: 44100 Hz, 128 frames/block, int16 stereo.
 *  Build (device): aarch64-linux-gnu-gcc -O2 -fPIC -shared -o dsp.so maze_voice.c -lm
 *  Build (PC test): cc -O2 -DLABYRINTH_STANDALONE -o maze_test maze_voice.c -lm && ./maze_test
 * ==========================================================================*/

#define _GNU_SOURCE          /* strtok_r under a strict -std=cNN build */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLE_RATE      44100.0f
#define FRAMES_PER_BLOCK 128
#define TWO_PI           6.28318530718f
#define PI_F             3.14159265359f
#define DENORMAL_GUARD   1e-20f

#ifndef OVERSAMPLE
#define OVERSAMPLE 2
#endif

#define EG_PITCH_SEMIS 48.0f
#define EG_CUT_OCT      5.0f
#define FM_MAX          3.0f
#define NOISE_FLOOR     1.12e-4f   /* ~ -79 dB */
#define MIX_HEADROOM    1.5f       /* per-channel warm-sat headroom */
#define MIX_ASYM        0.15f      /* per-channel asymmetry (even harmonics) */
#define BOSS_BIAS       0.28f      /* Boss-style asymmetric clip bias */
#define BOSS_DRIVE_MAX  6.0f       /* gain at full Tone/Sat */
#define BOSS_MAKEUP     1.3f
#define BOSS_TONE_HZ    3800.0f    /* Boss passive tone stage corner */

/* v0.3.2 exponential control-curve endpoints */
#define CUT_MIN_HZ      20.0f
#define CUT_MAX_HZ      20000.0f
#define DECAY_MIN_S     0.005f     /* 5 ms  */
#define DECAY_MAX_S     2.0f       /* 2 s   */

/* ------------------------------ DSP helpers ------------------------------ */
static inline float clampf(float x, float lo, float hi){
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float fast_tanh(float x){
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float poly_blep(float t, float dt){
    if (t < dt){ t /= dt; return t + t - t*t - 1.0f; }
    if (t > 1.0f - dt){ t = (t - 1.0f)/dt; return t*t + t + t + 1.0f; }
    return 0.0f;
}
static inline float frand(uint32_t* s){
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (float)(int32_t)(*s) * (1.0f / 2147483648.0f);
}
static inline float smooth1(float cur, float target, float coeff){
    return cur + coeff * (target - cur);
}
/* v0.3.2 exponential control curves (knob domain 0..1) */
static inline float cutoff_hz(float k01){
    return CUT_MIN_HZ * powf(CUT_MAX_HZ/CUT_MIN_HZ, clampf(k01,0.0f,1.0f));
}
static inline float decay_sec(float k01){
    return DECAY_MIN_S * powf(DECAY_MAX_S/DECAY_MIN_S, clampf(k01,0.0f,1.0f));
}

typedef struct { float x1, y1; } dcblock_t;
static inline float dcblock(dcblock_t* d, float x){
    float y = x - d->x1 + 0.9975f * d->y1;
    d->x1 = x; d->y1 = y + DENORMAL_GUARD - DENORMAL_GUARD;
    return y;
}
typedef struct { float x_prev; } sat_t;
static inline float shape3(sat_t* s, float x, float (*fn)(float)){
    float xm = 0.5f * (x + s->x_prev);
    float y  = (fn(s->x_prev) + 4.0f * fn(xm) + fn(x)) / 6.0f;
    s->x_prev = x;
    return y;
}
static inline float sat_process(sat_t* s, float x){
    float xm = 0.5f * (x + s->x_prev);
    float y  = (fast_tanh(s->x_prev) + 4.0f * fast_tanh(xm) + fast_tanh(x)) / 6.0f;
    s->x_prev = x;
    return y;
}
static inline float warm_transfer(float u){
    return MIX_HEADROOM * fast_tanh((u + MIX_ASYM) / MIX_HEADROOM)
         - MIX_HEADROOM * fast_tanh(MIX_ASYM / MIX_HEADROOM);
}
static inline float warm_shaped(sat_t* s, float u){ return shape3(s, u, warm_transfer); }
static inline float boss_transfer(float x){
    return fast_tanh(x + BOSS_BIAS) - fast_tanh(BOSS_BIAS);
}
static inline float boss_shaped(sat_t* s, float x){ return shape3(s, x, boss_transfer); }

static inline float wavefold(float x){
    x = clampf(x, -8.0f, 8.0f);
    while (x >  1.0f) x =  2.0f - x;
    while (x < -1.0f) x = -2.0f - x;
    return fast_tanh(1.6f * x) * (1.0f / 0.9217f);
}
typedef struct { float ic1, ic2; float g, k, morph; } svf_t;
static inline void svf_set(svf_t* f, float fc, float Q, float morph, float fs){
    fc = clampf(fc, 20.0f, 0.45f * fs);
    f->g = tanf(PI_F * fc / fs);
    f->k = 1.0f / clampf(Q, 0.5f, 12.0f);
    f->morph = morph;
}
static inline float svf_tick(svf_t* f, float x){
    float k_nl = f->k * (1.0f + 0.15f * fast_tanh(fabsf(f->ic1)));
    float a1 = 1.0f / (1.0f + f->g * (f->g + k_nl));
    float a2 = f->g * a1;
    float a3 = f->g * a2;
    float v3 = x - f->ic2;
    float v1 = a1 * f->ic1 + a2 * v3;
    float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    return (1.0f - f->morph) * v2 + f->morph * v1;
}

/* Decay-only envelope with a 2 ms attack ramp (removes retrigger clicks). */
typedef struct { float level, coef, atk, atkInc; } env_t;
static inline void env_set(env_t* e, float sec, float fs){
    e->coef = expf(-1.0f / (clampf(sec, 0.005f, 5.0f) * fs));
}
static inline void env_trig(env_t* e){
    e->atk = 1.0f;
    e->atkInc = (1.0f - e->level) / (0.002f * SAMPLE_RATE);
    if (e->atkInc < 0.0f) e->atkInc = 0.0f;
}
static inline float env_run(env_t* e){
    if (e->atk > 0.0f){
        e->level += e->atkInc;
        if (e->level >= 1.0f){ e->level = 1.0f; e->atk = 0.0f; }
    } else {
        e->level *= e->coef;
    }
    return e->level;
}

/* ============================================================================
 *  Instance
 * ==========================================================================*/
typedef struct {
    double phase1, phaseMod;
    float  err1, errMod;
    sat_t  satVco, satMod, satNoise;
    dcblock_t dcMix;
    sat_t  satBoss;
    float  bossLP; float bossToneA;
    sat_t  satOut;
    svf_t  svf;
    dcblock_t dcFold, dcOut;
    float  noiseLP;
    uint32_t rng;
    struct { float b0,b1,b2,a1,a2,z1,z2; } dec;

    env_t  eg1, eg2;

    float  drift1, driftMod;
    float  note, vel;
    float  fsInternal;

    /* internal-domain params */
    float vcoTune;   /* semitones -24..24 */
    float modFreq;   /* Hz */
    float fmDepth;   /* 0..1 */
    float fmEg1;     /* -1..1 */
    float vcoEg1, vcoKey, modEg1, modKey;
    float env2Decay; /* v0.3.2: 0..1 knob (exp -> seconds) */
    float foldDrive, foldBias, foldEg1, foldKey, blend;
    int   route;
    float cutoff;    /* v0.3.2: 0..1 knob (exp -> Hz) */
    float reso, filterMode, env1Decay, cutoffEg1, cutoffKey;
    float vcoLvl, modLvl, noiseLvl;   /* 0..2 */
    float noiseTone;                  /* -1..1 */
    float ringLvl;                    /* 0..1 */
    float sat;                        /* 0..1 */
    float level;                      /* 0..1 */

    /* v0.3.4 randomise-page toggles (0/1) */
    int rndVoice, rndWavefolder, rndFilter, rndTone;

    /* smoothed */
    float sVcoLvl, sModLvl, sNoiseLvl, sNoiseTone, sRingLvl, sSat, sLevel;
    float sBlend, sFoldBias, sFoldDrive, sFmDepth;

    char  err[96];
} maze_t;

static const void* g_host = NULL;
static uint32_t g_rrng = 0x9e3779b9u;      /* randomiser RNG */

static inline float rr(float lo, float hi){
    g_rrng ^= g_rrng<<13; g_rrng ^= g_rrng>>17; g_rrng ^= g_rrng<<5;
    float u = (g_rrng & 0xFFFFFFu) / (float)0x1000000;
    return lo + (hi-lo)*u;
}

/* ---------- module.json self-read (forge.c pattern) ---------------------- */
static char g_module_dir[512] = {0};
static char *g_chain_params_cache = NULL;
static char *g_ui_hierarchy_cache = NULL;
static int   g_module_json_attempted = 0;

static char *extract_json_value(const char *json, const char *key){
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char open = *p;
    if (open != '{' && open != '[') return NULL;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    const char *start = p;
    while (*p){
        if (*p == open) depth++;
        else if (*p == close){
            depth--;
            if (depth == 0){
                int len = (int)(p - start) + 1;
                char *out = (char *)malloc((size_t)len + 1);
                if (!out) return NULL;
                memcpy(out, start, (size_t)len);
                out[len] = '\0';
                return out;
            }
        }
        p++;
    }
    return NULL;
}
static void load_module_json(void){
    if (g_module_json_attempted) return;
    g_module_json_attempted = 1;
    if (!g_module_dir[0]) return;
    char path[768];
    snprintf(path, sizeof(path), "%s/module.json", g_module_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024){ fclose(f); return; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf){ fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    g_chain_params_cache = extract_json_value(buf, "chain_params");
    g_ui_hierarchy_cache = extract_json_value(buf, "ui_hierarchy");
    free(buf);
}

static void biquad_lp_init(maze_t* v, float fc, float fs, float Q){
    float w=2.0f*PI_F*fc/fs, a=sinf(w)/(2.0f*Q), cc=cosf(w);
    float b0=(1-cc)*0.5f,b1=1-cc,b2=(1-cc)*0.5f,a0=1+a,a1=-2*cc,a2=1-a;
    v->dec.b0=b0/a0;v->dec.b1=b1/a0;v->dec.b2=b2/a0;v->dec.a1=a1/a0;v->dec.a2=a2/a0;
    v->dec.z1=v->dec.z2=0;
}
static inline float biquad_run(maze_t* v, float x){
    float y=v->dec.b0*x+v->dec.z1;
    v->dec.z1=v->dec.b1*x-v->dec.a1*y+v->dec.z2;
    v->dec.z2=v->dec.b2*x-v->dec.a2*y;
    return y;
}

static void load_defaults(maze_t* v){
    v->err1 = 2.5f; v->errMod = -3.5f;
    v->rng  = 0x1234567u;
    v->fsInternal = SAMPLE_RATE * (float)OVERSAMPLE;
    v->note = 45.0f; v->vel = 1.0f;

    v->vcoTune=0.0f; v->modFreq=55.0f; v->fmDepth=0.0f; v->fmEg1=0.0f;
    v->vcoEg1=0.0f; v->vcoKey=1.0f; v->modEg1=0.0f; v->modKey=1.0f;
    v->env2Decay=0.70f;
    v->foldDrive=0.0f; v->foldBias=0.0f; v->route=1; v->foldEg1=0.0f; v->foldKey=0.0f; v->blend=0.0f;
    v->cutoff=1.0f;
    v->reso=0.0f; v->filterMode=0.0f; v->env1Decay=0.60f;
    v->cutoffEg1=0.3f; v->cutoffKey=0.0f;
    v->vcoLvl=1.0f; v->modLvl=0.5f; v->noiseLvl=0.0f; v->noiseTone=0.0f;
    v->ringLvl=0.0f; v->sat=0.0f; v->level=0.8f;

    /* v0.3.4: all randomise toggles armed by default */
    v->rndVoice=1; v->rndWavefolder=1; v->rndFilter=1; v->rndTone=1;

    v->sVcoLvl=v->vcoLvl; v->sModLvl=v->modLvl; v->sNoiseLvl=v->noiseLvl;
    v->sNoiseTone=v->noiseTone; v->sRingLvl=v->ringLvl; v->sSat=v->sat; v->sLevel=v->level;
    v->sBlend=v->blend; v->sFoldBias=v->foldBias; v->sFoldDrive=v->foldDrive; v->sFmDepth=v->fmDepth;

    v->bossToneA = 1.0f - expf(-TWO_PI * BOSS_TONE_HZ / v->fsInternal);

    env_set(&v->eg1, decay_sec(v->env1Decay), SAMPLE_RATE);
    env_set(&v->eg2, decay_sec(v->env2Decay), SAMPLE_RATE);
    biquad_lp_init(v, 19000.0f, v->fsInternal, 0.707f);
}

/* ---- randomise: page 0 Voice, 1 WaveFolder, 2 Filter, 3 Tone ------------ */
static void randomize_page(maze_t* v, int page){
    switch (page){
    case 0:
        v->vcoTune = (float)((int)rr(-7,7));
        v->modFreq = rr(30,300);
        v->fmDepth = rr(0,0.6f); v->fmEg1 = rr(-0.5f,0.5f);
        v->vcoEg1 = rr(-0.3f,0.3f); v->modEg1 = rr(-0.5f,0.5f);
        v->env2Decay = rr(0.35f,0.85f); env_set(&v->eg2, decay_sec(v->env2Decay), SAMPLE_RATE);
        break;
    case 1:
        v->foldDrive = rr(0,0.8f); v->foldBias = rr(-0.6f,0.6f);
        v->route = (int)rr(0,2.99f);
        v->foldEg1 = rr(-0.6f,0.6f); v->foldKey = rr(0,0.4f); v->blend = rr(-1,1);
        break;
    case 2:
        v->cutoff = rr(0.3f,1.0f); v->reso = rr(0,0.7f); v->filterMode = rr(0,1);
        v->env1Decay = rr(0.3f,0.85f); env_set(&v->eg1, decay_sec(v->env1Decay), SAMPLE_RATE);
        v->cutoffEg1 = rr(-0.6f,0.8f); v->cutoffKey = rr(0,0.5f);
        break;
    case 3:
        v->vcoLvl = rr(0.6f,1.6f); v->modLvl = rr(0,1.2f); v->noiseLvl = rr(0,0.6f);
        v->noiseTone = rr(-0.8f,0.8f); v->ringLvl = rr(0,0.6f); v->sat = rr(0,0.5f);
        v->level = rr(0.6f,0.9f);
        break;
    default: break;
    }
}
/* randomise every page whose toggle is armed */
static void randomize_armed(maze_t* v){
    if (v->rndVoice)      randomize_page(v, 0);
    if (v->rndWavefolder) randomize_page(v, 1);
    if (v->rndFilter)     randomize_page(v, 2);
    if (v->rndTone)       randomize_page(v, 3);
}
static inline int on_off(const char* val){
    return (!strcmp(val,"on") || !strcmp(val,"1") || atof(val) >= 0.5f) ? 1 : 0;
}
static inline int trig_fired(const char* v){ return v && strcmp(v,"off") && strcmp(v,"0"); }

/* ------------------------------ lifecycle -------------------------------- */
static void* create_instance(const char* module_dir, const char* json_defaults){
    (void)json_defaults;
    if (module_dir && *module_dir){
        strncpy(g_module_dir, module_dir, sizeof(g_module_dir) - 1);
        g_module_dir[sizeof(g_module_dir) - 1] = '\0';
    }
    maze_t* v = (maze_t*)calloc(1, sizeof(maze_t));
    if (!v) return NULL;
    load_defaults(v);
    return v;
}
static void destroy_instance(void* instance){ if (instance) free(instance); }

static void on_midi(void* instance, const uint8_t* msg, int len, int source){
    (void)source;
    maze_t* v = (maze_t*)instance;
    if (!v || len < 3) return;
    uint8_t st = msg[0] & 0xF0, d1 = msg[1], d2 = msg[2];
    /* Move sends note-on/off 0..9 for capacitive knob touch; raw_midi:true
     * means the host does not filter them for us. Ignore that range so a
     * knob touch cannot retrigger the voice. */
    if ((st == 0x90 || st == 0x80) && d1 <= 9) return;
    if (st == 0x90 && d2 > 0){
        v->note = (float)d1;
        v->vel  = 0.2f + 0.8f * (d2 / 127.0f);
        env_trig(&v->eg1);
        env_trig(&v->eg2);
    } else if (st == 0x80 || (st == 0x90 && d2 == 0)){
        /* decay-only EGs: note-off is a no-op */
    }
}

/* ---- set_param: DISPLAY value from host -> INTERNAL normalized ---------- */
static void set_param(void* instance, const char* key, const char* val){
    maze_t* v = (maze_t*)instance;
    if (!v || !key || !val) return;

    if (!strcmp(key,"state")){                       /* preset restore */
        char tmp[1200];
        strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
        char* save=NULL;
        for (char* tok=strtok_r(tmp,",",&save); tok; tok=strtok_r(NULL,",",&save)){
            char* eq=strchr(tok,'='); if(!eq) continue;
            *eq='\0'; set_param(instance, tok, eq+1);
        }
        return;
    }

    /* v0.3.4 randomise page */
    if (!strcmp(key,"rnd_voice"))      { v->rndVoice      = on_off(val); return; }
    if (!strcmp(key,"rnd_wavefolder")) { v->rndWavefolder = on_off(val); return; }
    if (!strcmp(key,"rnd_filter"))     { v->rndFilter     = on_off(val); return; }
    if (!strcmp(key,"rnd_tone"))       { v->rndTone       = on_off(val); return; }
    if (!strcmp(key,"rnd_go")){                      /* momentary Generate */
        if (trig_fired(val)) randomize_armed(v);
        return;
    }
    if (!strcmp(key,"randomize")){                   /* scripting fallback */
        if      (!strcmp(val,"voice"))      randomize_page(v,0);
        else if (!strcmp(val,"wavefolder")) randomize_page(v,1);
        else if (!strcmp(val,"filter"))     randomize_page(v,2);
        else if (!strcmp(val,"tone"))       randomize_page(v,3);
        else                                randomize_armed(v);   /* "armed"/other */
        return;
    }

    float f = (float)atof(val);
    if      (!strcmp(key,"vco_tune"))   v->vcoTune  = f;
    else if (!strcmp(key,"mod_freq"))   v->modFreq  = f;
    else if (!strcmp(key,"fm_depth"))   v->fmDepth  = f/100.0f;
    else if (!strcmp(key,"fm_eg1"))     v->fmEg1    = f/100.0f;
    else if (!strcmp(key,"vco_eg1"))    v->vcoEg1   = f/100.0f;
    else if (!strcmp(key,"mod_eg1"))    v->modEg1   = f/100.0f;
    else if (!strcmp(key,"vco_key"))    v->vcoKey   = f/100.0f;
    else if (!strcmp(key,"mod_key"))    v->modKey   = f/100.0f;
    else if (!strcmp(key,"env2_decay")){v->env2Decay=f/100.0f; env_set(&v->eg2,decay_sec(v->env2Decay),SAMPLE_RATE); }
    else if (!strcmp(key,"fold_drive")) v->foldDrive= f/100.0f;
    else if (!strcmp(key,"fold_bias"))  v->foldBias = f/100.0f;
    else if (!strcmp(key,"route"))      v->route    = (int)clampf(f,0,2);
    else if (!strcmp(key,"fold_eg1"))   v->foldEg1  = f/100.0f;
    else if (!strcmp(key,"fold_key"))   v->foldKey  = f/100.0f;
    else if (!strcmp(key,"blend"))      v->blend    = f/100.0f;
    else if (!strcmp(key,"cutoff"))     v->cutoff   = f/100.0f;
    else if (!strcmp(key,"reso"))       v->reso     = f/100.0f;
    else if (!strcmp(key,"filter_mode"))v->filterMode=f/100.0f;
    else if (!strcmp(key,"env1_decay")){v->env1Decay=f/100.0f; env_set(&v->eg1,decay_sec(v->env1Decay),SAMPLE_RATE); }
    else if (!strcmp(key,"cutoff_eg1")) v->cutoffEg1= f/100.0f;
    else if (!strcmp(key,"cutoff_key")) v->cutoffKey= f/100.0f;
    else if (!strcmp(key,"vco_lvl"))    v->vcoLvl   = f/100.0f;
    else if (!strcmp(key,"mod_lvl"))    v->modLvl   = f/100.0f;
    else if (!strcmp(key,"noise_lvl"))  v->noiseLvl = f/100.0f;
    else if (!strcmp(key,"noise_tone")) v->noiseTone= f/100.0f;
    else if (!strcmp(key,"ring_lvl"))   v->ringLvl  = f/100.0f;
    else if (!strcmp(key,"sat"))        v->sat      = f/100.0f;
    else if (!strcmp(key,"level"))      v->level    = f/100.0f;
}

/* serialise all params (display units) as "k=v,k=v,..." */
static int build_state(maze_t* v, char* buf, int len){
    return snprintf(buf, (size_t)len,
      "vco_tune=%.4g,mod_freq=%.4g,fm_depth=%.4g,fm_eg1=%.4g,vco_eg1=%.4g,mod_eg1=%.4g,env2_decay=%.4g,"
      "fold_drive=%.4g,fold_bias=%.4g,route=%d,fold_eg1=%.4g,fold_key=%.4g,blend=%.4g,"
      "cutoff=%.4g,reso=%.4g,filter_mode=%.4g,env1_decay=%.4g,cutoff_eg1=%.4g,cutoff_key=%.4g,"
      "vco_lvl=%.4g,mod_lvl=%.4g,noise_lvl=%.4g,noise_tone=%.4g,ring_lvl=%.4g,sat=%.4g,level=%.4g,"
      "vco_key=%.4g,mod_key=%.4g,"
      "rnd_voice=%d,rnd_wavefolder=%d,rnd_filter=%d,rnd_tone=%d",
      v->vcoTune, v->modFreq, v->fmDepth*100, v->fmEg1*100, v->vcoEg1*100, v->modEg1*100, v->env2Decay*100,
      v->foldDrive*100, v->foldBias*100, v->route, v->foldEg1*100, v->foldKey*100, v->blend*100,
      v->cutoff*100, v->reso*100, v->filterMode*100, v->env1Decay*100, v->cutoffEg1*100, v->cutoffKey*100,
      v->vcoLvl*100, v->modLvl*100, v->noiseLvl*100, v->noiseTone*100, v->ringLvl*100, v->sat*100, v->level*100,
      v->vcoKey*100, v->modKey*100,
      v->rndVoice, v->rndWavefolder, v->rndFilter, v->rndTone);
}

/* ---- get_param: INTERNAL -> DISPLAY (plus self-serve + state) ----------- */
static int get_param(void* instance, const char* key, char* buf, int buf_len){
    if (key && !strcmp(key, "module_id"))
        return snprintf(buf, (size_t)buf_len, "maze-voice");
    if (key && (!strcmp(key, "chain_params") || !strcmp(key, "ui_hierarchy"))){
        load_module_json();
        const char *src = !strcmp(key, "chain_params") ? g_chain_params_cache
                                                         : g_ui_hierarchy_cache;
        if (!src) return -1;
        return snprintf(buf, (size_t)buf_len, "%s", src);
    }
    maze_t* v = (maze_t*)instance;
    if (!v || !key || !buf || buf_len < 1) return -1;

    if (!strcmp(key,"state")) return build_state(v, buf, buf_len);

    /* v0.3.4 randomise page reads */
    if (!strcmp(key,"rnd_voice"))      return snprintf(buf, buf_len, "%d", v->rndVoice);
    if (!strcmp(key,"rnd_wavefolder")) return snprintf(buf, buf_len, "%d", v->rndWavefolder);
    if (!strcmp(key,"rnd_filter"))     return snprintf(buf, buf_len, "%d", v->rndFilter);
    if (!strcmp(key,"rnd_tone"))       return snprintf(buf, buf_len, "%d", v->rndTone);
    if (!strcmp(key,"rnd_go"))         return snprintf(buf, buf_len, "off");  /* momentary: always idle */

    float f = 0.0f; int ok = 1;
    if      (!strcmp(key,"vco_tune"))   f = v->vcoTune;
    else if (!strcmp(key,"mod_freq"))   f = v->modFreq;
    else if (!strcmp(key,"fm_depth"))   f = v->fmDepth*100.0f;
    else if (!strcmp(key,"fm_eg1"))     f = v->fmEg1*100.0f;
    else if (!strcmp(key,"vco_eg1"))    f = v->vcoEg1*100.0f;
    else if (!strcmp(key,"mod_eg1"))    f = v->modEg1*100.0f;
    else if (!strcmp(key,"vco_key"))    f = v->vcoKey*100.0f;
    else if (!strcmp(key,"mod_key"))    f = v->modKey*100.0f;
    else if (!strcmp(key,"env2_decay")) f = v->env2Decay*100.0f;
    else if (!strcmp(key,"fold_drive")) f = v->foldDrive*100.0f;
    else if (!strcmp(key,"fold_bias"))  f = v->foldBias*100.0f;
    else if (!strcmp(key,"route"))      f = (float)v->route;
    else if (!strcmp(key,"fold_eg1"))   f = v->foldEg1*100.0f;
    else if (!strcmp(key,"fold_key"))   f = v->foldKey*100.0f;
    else if (!strcmp(key,"blend"))      f = v->blend*100.0f;
    else if (!strcmp(key,"cutoff"))     f = v->cutoff*100.0f;
    else if (!strcmp(key,"reso"))       f = v->reso*100.0f;
    else if (!strcmp(key,"filter_mode"))f = v->filterMode*100.0f;
    else if (!strcmp(key,"env1_decay")) f = v->env1Decay*100.0f;
    else if (!strcmp(key,"cutoff_eg1")) f = v->cutoffEg1*100.0f;
    else if (!strcmp(key,"cutoff_key")) f = v->cutoffKey*100.0f;
    else if (!strcmp(key,"vco_lvl"))    f = v->vcoLvl*100.0f;
    else if (!strcmp(key,"mod_lvl"))    f = v->modLvl*100.0f;
    else if (!strcmp(key,"noise_lvl"))  f = v->noiseLvl*100.0f;
    else if (!strcmp(key,"noise_tone")) f = v->noiseTone*100.0f;
    else if (!strcmp(key,"ring_lvl"))   f = v->ringLvl*100.0f;
    else if (!strcmp(key,"sat"))        f = v->sat*100.0f;
    else if (!strcmp(key,"level"))      f = v->level*100.0f;
    else ok = 0;
    if (!ok) return -1;
    return snprintf(buf, buf_len, "%.4f", f);
}

static int get_error(void* instance, char* buf, int buf_len){
    maze_t* v = (maze_t*)instance;
    if (!v || !buf || buf_len < 1) return 0;
    if (v->err[0] == '\0') return 0;
    return snprintf(buf, buf_len, "%s", v->err);
}

/* ============================================================================
 *  Audio-rate voice  (UNCHANGED from v0.3.1)
 * ==========================================================================*/
typedef struct {
    float f1, fMod, fmIndex;
    float gVco, gMod, gRing, gNoise;
    float satAmt;
    float foldBias, blend, amp, level;
    int   route;
} ctrl_t;

static inline float voice_tick(maze_t* v, const ctrl_t* c, float foldGain){
    float fs = v->fsInternal;

    float incMod = c->fMod / fs;
    v->phaseMod += incMod;
    if (v->phaseMod >= 1.0) v->phaseMod -= 1.0;
    if (v->phaseMod <  0.0) v->phaseMod += 1.0;
    float tp = (float)v->phaseMod;
    float tri = 4.0f * fabsf(tp - 0.5f) - 1.0f;
    tri -= poly_blep(tp, incMod<0?-incMod:incMod) * 0.5f;

    float inc1 = (c->f1 * (1.0f + c->fmIndex * tri)) / fs;
    v->phase1 += inc1;
    if (v->phase1 >= 1.0) v->phase1 -= 1.0;
    if (v->phase1 <  0.0) v->phase1 += 1.0;
    float osc1 = sinf(TWO_PI * (float)v->phase1);

    float w = frand(&v->rng);
    v->noiseLP += 0.22f * (w - v->noiseLP);
    float hp = w - v->noiseLP;
    float tonePos = 0.5f + 0.5f * v->sNoiseTone;
    float noise = (1.0f - tonePos) * v->noiseLP + tonePos * hp;

    float ring = osc1 * tri;

    float vcoC = warm_shaped(&v->satVco,   c->gVco   * osc1);
    float modC = warm_shaped(&v->satMod,   c->gMod   * tri);
    float noiC = warm_shaped(&v->satNoise, c->gNoise * noise);
    float premix = dcblock(&v->dcMix, vcoC + modC + noiC);
    float core = premix + ring * c->gRing;

    float folded, filtered;
    if (c->route == 1){
        folded   = wavefold(foldGain * core + c->foldBias);
        filtered = svf_tick(&v->svf, core);
    } else if (c->route == 0){
        folded   = wavefold(foldGain * core + c->foldBias);
        filtered = svf_tick(&v->svf, folded);
    } else {
        filtered = svf_tick(&v->svf, core);
        folded   = wavefold(foldGain * filtered + c->foldBias);
    }
    folded = dcblock(&v->dcFold, folded);

    float blendPos = 0.5f + 0.5f * c->blend;
    float mix = (1.0f - blendPos) * folded + blendPos * filtered;

    float outp = sat_process(&v->satOut, 1.1f * mix);
    outp = dcblock(&v->dcOut, outp);
    outp *= c->amp;

    if (c->satAmt > 0.0001f){
        float drive = 1.0f + c->satAmt * BOSS_DRIVE_MAX;
        float wet = boss_shaped(&v->satBoss, drive * outp);
        v->bossLP += v->bossToneA * (wet - v->bossLP);
        wet = v->bossLP * BOSS_MAKEUP;
        outp = (1.0f - c->satAmt) * outp + c->satAmt * wet;
    }

    outp *= c->level;
    outp += NOISE_FLOOR * frand(&v->rng);
    return outp;
}

static void render_block(void* instance, int16_t* out_lr, int frames){
    maze_t* v = (maze_t*)instance;
    if (!v){ memset(out_lr, 0, sizeof(int16_t)*2*frames); return; }

    v->drift1   += 0.0006f * (frand(&v->rng) - v->drift1);
    v->driftMod += 0.0006f * (frand(&v->rng) - v->driftMod);
    float dC1 = 4.0f * v->drift1 + v->err1;
    float dCM = 4.0f * v->driftMod + v->errMod;

    const float sc = 0.25f;
    v->sVcoLvl   = smooth1(v->sVcoLvl,  v->vcoLvl,  sc);
    v->sModLvl   = smooth1(v->sModLvl,  v->modLvl,  sc);
    v->sNoiseLvl = smooth1(v->sNoiseLvl,v->noiseLvl,sc);
    v->sNoiseTone= smooth1(v->sNoiseTone,v->noiseTone,sc);
    v->sRingLvl  = smooth1(v->sRingLvl, v->ringLvl, sc);
    v->sSat      = smooth1(v->sSat,     v->sat,     sc);
    v->sLevel    = smooth1(v->sLevel,   v->level,   sc);
    v->sBlend    = smooth1(v->sBlend,   v->blend,   sc);
    v->sFoldBias = smooth1(v->sFoldBias,v->foldBias,sc);
    v->sFoldDrive= smooth1(v->sFoldDrive,v->foldDrive,sc);
    v->sFmDepth  = smooth1(v->sFmDepth, v->fmDepth, sc);

    float eg1b = v->eg1.level;

    float vcoBaseMidi = 69.0f + v->vcoKey * (v->note - 69.0f);
    float vcoSemis = v->vcoTune + v->vcoEg1 * eg1b * EG_PITCH_SEMIS;
    float f1 = 440.0f * powf(2.0f, (vcoBaseMidi - 69.0f + vcoSemis + dC1/100.0f)/12.0f);
    f1 = clampf(f1, 1.0f, 0.45f*v->fsInternal);

    float modKeyOff = v->note - 60.0f;
    float modSemis = v->modKey * modKeyOff + v->modEg1 * eg1b * EG_PITCH_SEMIS;
    float fMod = v->modFreq * powf(2.0f, (modSemis + dCM/100.0f)/12.0f);
    fMod = clampf(fMod, 0.05f, 0.45f*v->fsInternal);

    float cutKeyOff = v->note - 60.0f;
    float cutSemis = v->cutoffKey * cutKeyOff;
    float cutEnv   = v->cutoffEg1 * eg1b * EG_CUT_OCT;
    float cutHz = cutoff_hz(v->cutoff) * powf(2.0f, cutSemis/12.0f) * powf(2.0f, cutEnv);
    float Q = 0.5f + v->reso * v->reso * 11.5f;
    svf_set(&v->svf, cutHz, Q, v->filterMode, v->fsInternal);

    ctrl_t c;
    c.f1 = f1; c.fMod = fMod;
    c.fmIndex = clampf(v->sFmDepth + v->fmEg1 * eg1b, 0.0f, 1.0f) * FM_MAX;
    c.gVco   = v->sVcoLvl;
    c.gMod   = v->sModLvl;
    c.gNoise = v->sNoiseLvl;
    c.gRing  = v->sRingLvl;
    c.satAmt = v->sSat;
    c.foldBias = v->sFoldBias;
    c.blend = v->sBlend;
    c.level = v->sLevel;
    c.route = v->route;

    float foldKeyTerm = v->foldKey * ((v->note - 60.0f) / 24.0f);

    for (int i = 0; i < frames; ++i){
        float e1 = env_run(&v->eg1);
        float e2 = env_run(&v->eg2);
        c.amp = e2 * v->vel;

        float foldAmt = clampf(v->sFoldDrive + v->foldEg1 * e1 + foldKeyTerm, 0.0f, 1.0f);
        float foldGain = 1.0f + foldAmt * 7.0f;

        float s;
#if OVERSAMPLE > 1
        s = 0.0f;
        for (int os = 0; os < OVERSAMPLE; ++os)
            s = biquad_run(v, voice_tick(v, &c, foldGain));
#else
        s = voice_tick(v, &c, foldGain);
#endif
        int q = (int)lrintf(clampf(s, -1.0f, 1.0f) * 32767.0f);
        if (q >  32767) q =  32767;
        if (q < -32768) q = -32768;
        out_lr[2*i]   = (int16_t)q;
        out_lr[2*i+1] = (int16_t)q;
    }
}

/* ============================================================================
 *  API v2 export
 * ==========================================================================*/
typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char*, const char*);
    void  (*destroy_instance)(void*);
    void  (*on_midi)(void*, const uint8_t*, int, int);
    void  (*set_param)(void*, const char*, const char*);
    int   (*get_param)(void*, const char*, char*, int);
    int   (*get_error)(void*, char*, int);
    void  (*render_block)(void*, int16_t*, int);
} plugin_api_v2_t;

__attribute__((visibility("default")))
plugin_api_v2_t* move_plugin_init_v2(const void* host){
    g_host = host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = get_error,
        .render_block     = render_block,
    };
    return &api;
}

/* ============================================================================
 *  PC-ONLY TEST HARNESS
 * ==========================================================================*/
#ifdef LABYRINTH_STANDALONE
static void write_wav(const char* path, const int16_t* buf, int n, int sr){
    FILE* f=fopen(path,"wb"); if(!f){perror("fopen");return;}
    int byteRate=sr*2,dataLen=n*2,riffLen=36+dataLen;
    uint16_t one=1,bpf=2,bits=16,chan=1; uint32_t fmtLen=16,srate=sr,br=byteRate;
    fwrite("RIFF",1,4,f);fwrite(&riffLen,4,1,f);fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f);fwrite(&fmtLen,4,1,f);fwrite(&one,2,1,f);fwrite(&chan,2,1,f);
    fwrite(&srate,4,1,f);fwrite(&br,4,1,f);fwrite(&bpf,2,1,f);fwrite(&bits,2,1,f);
    fwrite("data",1,4,f);fwrite(&dataLen,4,1,f);
    fwrite(buf,sizeof(int16_t),n,f); fclose(f);
}
static void sp(void* inst, const char* k, float val){ char b[32]; snprintf(b,sizeof b,"%.4f",val); set_param(inst,k,b); }
int main(void){
    const int sr=(int)SAMPLE_RATE, blocks=(int)(6.0f*sr/FRAMES_PER_BLOCK);
    const int N=blocks*FRAMES_PER_BLOCK;
    int16_t* st=(int16_t*)malloc(sizeof(int16_t)*2*FRAMES_PER_BLOCK);
    int16_t* mono=(int16_t*)malloc(sizeof(int16_t)*N);
    void* inst=create_instance(".",NULL);

    char b[64];
    printf("module_id:    %d\n", get_param(inst,"module_id",b,sizeof b));
    char* cp=(char*)malloc(8192);
    printf("chain_params: %d  ui_hierarchy: %d\n",
           get_param(inst,"chain_params",cp,8192), get_param(inst,"ui_hierarchy",cp,8192));

    /* preset round-trip incl. toggles */
    sp(inst,"cutoff",40); sp(inst,"reso",55); sp(inst,"vco_lvl",140);
    set_param(inst,"rnd_tone","off");
    char state1[1200]; get_param(inst,"state",state1,sizeof state1);
    sp(inst,"cutoff",90); set_param(inst,"rnd_tone","on");
    set_param(inst,"state",state1);
    char chk[32]; get_param(inst,"cutoff",chk,sizeof chk);
    get_param(inst,"rnd_tone",b,sizeof b);
    printf("preset restore: cutoff=%s rnd_tone=%s  %s\n", chk, b,
           (atof(chk)>39&&atof(chk)<41 && !strcmp(b,"0"))?"OK":"FAIL");

    printf("cutoff knob 0/50/100 -> %.1f / %.1f / %.1f Hz\n", cutoff_hz(0.0f),cutoff_hz(0.5f),cutoff_hz(1.0f));
    printf("decay  knob 0/50/100 -> %.3f / %.3f / %.3f s\n", decay_sec(0.0f),decay_sec(0.5f),decay_sec(1.0f));

    /* Randomise page: toggle only Filter+Tone, arm-generate, confirm ONLY those move */
    set_param(inst,"rnd_voice","off"); set_param(inst,"rnd_wavefolder","off");
    set_param(inst,"rnd_filter","on"); set_param(inst,"rnd_tone","on");
    sp(inst,"vco_tune",5); sp(inst,"cutoff",50); sp(inst,"vco_lvl",111);
    char vt0[16],cut0[16],vl0[16]; get_param(inst,"vco_tune",vt0,16); get_param(inst,"cutoff",cut0,16); get_param(inst,"vco_lvl",vl0,16);
    set_param(inst,"rnd_go","go");                 /* press Generate */
    char vt1[16],cut1[16],vl1[16]; get_param(inst,"vco_tune",vt1,16); get_param(inst,"cutoff",cut1,16); get_param(inst,"vco_lvl",vl1,16);
    printf("Generate (Filter+Tone armed):\n");
    printf("  vco_tune (Voice, OFF): %s -> %s  %s\n", vt0,vt1, strcmp(vt0,vt1)==0?"unchanged OK":"CHANGED (bad)");
    printf("  cutoff   (Filter, ON): %s -> %s  %s\n", cut0,cut1, strcmp(cut0,cut1)!=0?"changed OK":"unchanged (bad)");
    printf("  vco_lvl  (Tone, ON):   %s -> %s  %s\n", vl0,vl1, strcmp(vl0,vl1)!=0?"changed OK":"unchanged (bad)");
    get_param(inst,"rnd_go",b,sizeof b);
    printf("  rnd_go idle read: %s\n", b);

    sp(inst,"cutoff",60); sp(inst,"reso",30); sp(inst,"vco_lvl",120); sp(inst,"mod_lvl",70);
    sp(inst,"env1_decay",60); sp(inst,"env2_decay",70); sp(inst,"level",80);
    int seq[]={45,52,57,60,57,52,48,45}; int steps=8; float stepDur=0.6f; int last=-1;
    for (int bk=0;bk<blocks;++bk){
        float t=(float)(bk*FRAMES_PER_BLOCK)/sr;
        int step=(int)(t/stepDur)%steps;
        if (step!=last){ uint8_t on[3]={0x90,(uint8_t)seq[step],105}; on_midi(inst,on,3,0); last=step; }
        render_block(inst,st,FRAMES_PER_BLOCK);
        for (int i=0;i<FRAMES_PER_BLOCK;++i) mono[bk*FRAMES_PER_BLOCK+i]=st[2*i];
    }
    int16_t peak=1; for(int i=0;i<N;++i){int16_t a=mono[i]<0?-mono[i]:mono[i]; if(a>peak)peak=a;}
    float g=0.89f*32767.0f/peak;
    for(int i=0;i<N;++i){int q=(int)lrintf(mono[i]*g); if(q>32767)q=32767; if(q<-32768)q=-32768; mono[i]=(int16_t)q;}
    write_wav("maze_voice_demo.wav",mono,N,sr);
    printf("Rendered maze_voice_demo.wav (%.1fs) peak=%d\n",(float)N/sr,peak);
    free(cp); destroy_instance(inst); free(st); free(mono);
    return 0;
}
#endif
