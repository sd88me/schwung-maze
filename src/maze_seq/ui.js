/* ============================================================================
 * MAZE  —  Overtake UI for Ableton Move (Schwung)            [id: maze_seq]
 * ----------------------------------------------------------------------------
 * Pads + step buttons + knobs + display. ALL sequencing lives in dsp.so
 * (maze_seq.c). This file draws the surface and forwards control changes.
 *
 * Hardware map (confirmed from the stock tb3po UI):
 *   16 step buttons = notes 16..31 (1-8 SEQ1 bits, 9-16 SEQ2 bits)
 *   Pad grid 4x8    = notes 68..99 (row0 68-75 bottom .. row3 92-99 top)
 *   Knobs (in)      = CC 71..78 (relative encoders)
 *   Knob LEDs (out) = CC 71..78 (indicator brightness / colour)
 *   Jog turn/click  = CC 14 / 3
 *   + / -           = CC 55 / 54
 *   Back / Shift    = CC 51 / 49
 *
 * v1.1 changes:
 *   - JOG WHEEL switches the two knob pages (was Track 1/2).
 *   - + / - move the pad KEYBOARD up/down an octave (reach higher/lower
 *     notes) instead of transposing the sequence directly.
 *   - Knob indicator LEDs: Move's knob LEDs are RGB palette (not brightness),
 *     so we light an in-use knob steady WHITE and leave free knobs off; the
 *     Trig Mix knob is two-coloured (red side / off centre / teal-blue side).
 *   - + / - shift the pad KEYBOARD by an octave WITHOUT changing the sounding
 *     transpose: the selected note keeps playing, its highlight just moves rows.
 *   - Page 1 renamed "SEQUENCERS"; Xpose shown on both pages; RUN/stop removed.
 * ==========================================================================*/

import { setLED as sharedSetLED, setButtonLED as sharedSetButtonLED }
    from '/data/UserData/schwung/shared/input_filter.mjs';

const NOTE_STEP_BASE = 16;
const NOTE_PAD_BASE  = 68;
const CC_KNOB_BASE   = 71;              /* knobs in AND indicator LEDs out */
const CC_JOG   = 14;
const CC_SHIFT = 49, CC_BACK = 51;
const CC_DOWN  = 54, CC_UP = 55;

/* LED colours (Move velocities). Value 8 = play-head colour on this unit. */
const LED_OFF=0, LED_RED=1, LED_HEAD=8, LED_GREEN=8, LED_ORANGE=47,
      LED_BLUE=95, LED_WHITE=120, LED_GREY=124, LED_TEAL=87;

/* ==>> EDIT ME: set false if your knob-indicator LEDs are white-only and the
   two-colour Trig Mix doesn't render — it will then use brightness-from-centre. */
const TRIG_MIX_TWO_COLOUR = true;

/* Scales — MUST match SCALES[] order in maze_seq.c */
const SCALES = [
  {name:"Chromatic", pc:[0,1,2,3,4,5,6,7,8,9,10,11]},
  {name:"Major",     pc:[0,2,4,5,7,9,11]},
  {name:"Minor",     pc:[0,2,3,5,7,8,10]},
  {name:"Pent Maj",  pc:[0,2,4,7,9]},
  {name:"Pent Min",  pc:[0,3,5,7,10]},
  {name:"Mel Min",   pc:[0,2,3,5,7,9,11]},
  {name:"Harm Min",  pc:[0,2,3,5,7,8,11]},
  {name:"Whole",     pc:[0,2,4,6,8,10]},
  {name:"Hirajoshi", pc:[0,2,3,7,8]},
  {name:"Major 7",   pc:[0,4,7,11]},
  {name:"Minor 7",   pc:[0,3,7,10]},
  {name:"Unquant",   pc:[0,1,2,3,4,5,6,7,8,9,10,11]}
];
const RATE_NAMES = ["1/32","1/16","1/8","1/4","1/2","1 bar"];
const GATE_NAMES = ["1/4","1/2","3/4","1","1 1/4","1 1/2","1 3/4","2"];
const NOTE_NAMES = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
/* Reset Both: BARS between play-head resets. Last index = Off.
   MUST match RESET_BARS[] in maze_seq.c. */
const RESET_LABELS = ["1b","2b","4b","8b","Off"];

/* UI owns everything except bit/play/running (those are polled from the DSP). */
const U = {
  page:0,
  s1_corrupt:0, s1_cv_range:20, s1_length:8, s1_channel:0,
  s2_corrupt:0, s2_cv_range:20, s2_length:8, s2_channel:0,
  trig_mix:0, scale:1, key:0, rate:1, gate:3, g_reset:4,   /* g_reset: index into RESET_LABELS; last = Off */
  pad_semis:0, padOct:0, padRow:2, padCol:0,   /* padOct = keyboard octave shift */
  s1_bits:[0,0,0,0,0,0,0,0], s1_play:-1,
  s2_bits:[0,0,0,0,0,0,0,0], s2_play:-1
};
let shiftHeld=false, pollTick=0;

function setP(k,v){ if (typeof host_module_set_param==="function") host_module_set_param(k,String(v)); }
function getP(k){ return (typeof host_module_get_param==="function") ? host_module_get_param(k) : null; }

function parseState(str, bits){
  if(!str) return {len:8,play:-1};
  const p=str.split("|"); if(p.length<2) return {len:8,play:-1};
  const len=parseInt(p[0],10)||8;
  const bs=p[1].split(",");
  for(let i=0;i<8&&i<bs.length;i++) bits[i]=(parseInt(bs[i],10)||0);
  const play=p.length>2?(parseInt(p[2],10)):-1;
  return {len,play};
}
function pollDsp(){
  const a=parseState(getP("s1_state"), U.s1_bits); U.s1_length=a.len; U.s1_play=a.play;
  const b=parseState(getP("s2_state"), U.s2_bits); U.s2_length=b.len; U.s2_play=b.play;
  pollTick++;
}

/* Push UI-owned params to the DSP (init + periodic) so a suspend/resume DSP
   reload can't silently revert them. */
function assertOwnedParams(){
  setP("scale",U.scale); setP("key",U.key);
  setP("note_rate",U.rate); setP("note_length",U.gate);
  setP("trig_mix",U.trig_mix);
  setP("g_reset",U.g_reset);
  setP("transpose","0");                 /* octave now lives in pad_semis */
  setP("pad_semis",U.pad_semis);
  setP("s1_corrupt",U.s1_corrupt);  setP("s1_cv_range",U.s1_cv_range);  setP("s1_length",U.s1_length);  setP("s1_channel",U.s1_channel);
  setP("s2_corrupt",U.s2_corrupt);  setP("s2_cv_range",U.s2_cv_range);  setP("s2_length",U.s2_length);  setP("s2_channel",U.s2_channel);
}

function decodeDelta(v){ if(v===0||v===64) return 0; return (v<=63)? v : -(128-v); }
function clamp(v,lo,hi){ return v<lo?lo:(v>hi?hi:v); }

function adjCont(key, prop, delta, lo, hi){
  const nv=clamp(U[prop]+delta, lo, hi);
  if(nv!==U[prop]){ U[prop]=nv; setP(key,nv); }
}
function adjStep(key, prop, delta, lo, hi){
  if(delta===0) return;
  const nv=clamp(U[prop]+(delta>0?1:-1), lo, hi);
  if(nv!==U[prop]){ U[prop]=nv; setP(key,nv); }
}

function handleKnob(idx, delta){
  if(delta===0) return;
  if(U.page===0){                          /* PAGE 1: SEQUENCERS */
    switch(idx){
      case 0: adjCont("s1_corrupt","s1_corrupt",delta,0,100); break;
      case 1: adjCont("s1_cv_range","s1_cv_range",delta,0,100); break;
      case 2: adjStep("s1_length","s1_length",delta,1,8); break;
      case 3: adjCont("trig_mix","trig_mix",delta,-63,64); break;
      case 4: adjCont("s2_corrupt","s2_corrupt",delta,0,100); break;
      case 5: adjCont("s2_cv_range","s2_cv_range",delta,0,100); break;
      case 6: adjStep("s2_length","s2_length",delta,1,8); break;
      case 7: adjStep("g_reset","g_reset",delta,0,RESET_LABELS.length-1); break;   /* Reset Both */
    }
  } else {                                 /* PAGE 2: GLOBAL */
    switch(idx){
      case 0: adjStep("scale","scale",delta,0,SCALES.length-1); break;
      case 1: adjStep("key","key",delta,0,11); break;
      case 2: adjStep("note_rate","rate",delta,0,RATE_NAMES.length-1); break;
      case 3: adjStep("note_length","gate",delta,0,GATE_NAMES.length-1); break;
      case 4: adjStep("s1_channel","s1_channel",delta,0,15); break;   /* knob 5 */
      case 5: adjStep("s2_channel","s2_channel",delta,0,15); break;   /* knob 6 */
      /* case 6,7: free */
    }
  }
}

/* ---- fixed SCALE-MODE keyboard (top 3 pad rows) --------------------------
 * Within a row = ascending scale degrees; each row up = +1 octave. HOME
 * (row 2, col 0) = root. + / - shift the whole keyboard by an octave via
 * padOct, so you can reach higher/lower registers with the same pads. */
const HOME_ROW = 2, HOME_COL = 0;
function degreeToSemitone(deg){
  const sc=SCALES[U.scale]; const n=sc.pc.length;
  const oct=Math.floor(deg/n), i=((deg%n)+n)%n;
  return oct*12 + sc.pc[i];
}
function padDegree(padRow, col){
  const n=SCALES[U.scale].pc.length;
  return (padRow-1)*n + col;
}
/* interval a pad represents = degrees-from-home + keyboard octave shift.
   Shifting padOct moves every pad's note by an octave, so the same note value
   appears on a different row (that's the "move the layout" behaviour). */
function padSemis(padRow, col){
  const base = degreeToSemitone(padDegree(padRow,col))
             - degreeToSemitone(padDegree(HOME_ROW,HOME_COL));
  return base + U.padOct*12;
}
/* After an octave shift, keep the highlight on the SAME sounding note by finding
   whichever pad now maps to U.pad_semis. Prefer the same column so the highlight
   tracks cleanly (7-note scales duplicate a note at col 7). Off-grid -> clear. */
function relocateHighlight(){
  const preferCol=U.padCol;
  let found=null;
  for(let row=1;row<=3;row++)
    for(let col=0;col<8;col++)
      if(padSemis(row,col)===U.pad_semis){
        if(col===preferCol){ U.padRow=row; U.padCol=col; return; }
        if(!found) found=[row,col];
      }
  if(found){ U.padRow=found[0]; U.padCol=found[1]; }
  else { U.padRow=-1; U.padCol=-1; }
}

globalThis.onMidiMessageInternal = function(data){
  if(!data) return;
  const st=data[0]|0, d1=data[1]|0, d2=data[2]|0, type=st&0xF0;

  if((type===0x90||type===0x80) && d1<10) return;   // knob/jog touch
  if(type===0xD0||type===0xA0) return;              // aftertouch
  if(st===0xF8||st===0xFA||st===0xFB||st===0xFC) return;

  if(type===0xB0 && d1===CC_SHIFT){ shiftHeld=(d2>0); return; }
  if(type===0xB0 && d1===CC_BACK && d2>0){ if(shiftHeld) setP("panic","1"); else setP("suspend","1"); return; }

  /* JOG WHEEL turns between the two knob pages */
  if(type===0xB0 && d1===CC_JOG){
    const dd=decodeDelta(d2);
    if(dd>0) U.page=1; else if(dd<0) U.page=0;
    return;
  }

  /* + / -  move the pad KEYBOARD by an octave. The sounding note (U.pad_semis)
     does NOT change — only the grid relabels, so the selected pad shifts rows.
     Up  -> notes move to higher rows (root climbs toward row 3).
     Down-> notes move to lower rows  (root drops toward row 1). */
  if(type===0xB0 && d1===CC_UP && d2>0){   U.padOct=clamp(U.padOct+1,-4,4); relocateHighlight(); return; }
  if(type===0xB0 && d1===CC_DOWN && d2>0){ U.padOct=clamp(U.padOct-1,-4,4); relocateHighlight(); return; }

  if(type===0xB0 && d1>=CC_KNOB_BASE && d1<CC_KNOB_BASE+8){ handleKnob(d1-CC_KNOB_BASE, decodeDelta(d2)); return; }

  /* 16 step buttons = sequencer bits */
  if(type===0x90 && d1>=NOTE_STEP_BASE && d1<NOTE_STEP_BASE+16 && d2>0){
    const i=d1-NOTE_STEP_BASE;
    if(i<8) setP("s1_flip", i); else setP("s2_flip", i-8);
    return;
  }

  /* pad grid */
  if(type===0x90 && d1>=NOTE_PAD_BASE && d1<=NOTE_PAD_BASE+31 && d2>0){
    const idx=d1-NOTE_PAD_BASE, row=Math.floor(idx/8), col=idx%8;
    if(row===0){                           /* bottom row: sequencer controls */
      switch(col){
        case 0: setP("s1_adv","-1"); break;
        case 1: setP("s1_adv","1");  break;
        case 2: setP("s1_len_dec","1"); break;
        case 4: setP("s2_adv","-1"); break;
        case 5: setP("s2_adv","1");  break;
        case 6: setP("s2_len_dec","1"); break;
        default: break;
      }
    } else {                               /* rows 1-3: keyboard */
      U.padRow=row; U.padCol=col;
      U.pad_semis = padSemis(row,col);
      setP("pad_semis", U.pad_semis);
    }
    return;
  }
};

/* ============================================================================
 *  Budgeted, diff-based LED updater (max ~14 sends/tick)
 * ==========================================================================*/
const shownLED = {};
function padNote(row,col){ return NOTE_PAD_BASE + row*8 + col; }

/* value fraction 0..1 for a knob's indicator; -1 = off/free; null = special */
function knobFrac(idx){
  if(U.page===0){
    switch(idx){
      case 0: return U.s1_corrupt/100;
      case 1: return U.s1_cv_range/100;
      case 2: return (U.s1_length-1)/7;
      case 3: return null;                 /* trig mix: coloured, handled below */
      case 4: return U.s2_corrupt/100;
      case 5: return U.s2_cv_range/100;
      case 6: return (U.s2_length-1)/7;
      case 7: return U.g_reset/(RESET_LABELS.length-1);
      default: return -1;
    }
  } else {
    switch(idx){
      case 0: return U.scale/(SCALES.length-1);
      case 1: return U.key/11;
      case 2: return U.rate/(RATE_NAMES.length-1);
      case 3: return U.gate/(GATE_NAMES.length-1);
      case 4: return U.s1_channel/15;
      case 5: return U.s2_channel/15;
      default: return -1;
    }
  }
}
/* Trig Mix indicator: 3-coloured. Red on the Seq1 (left) side, WHITE in the
   centre detent band (|t| <= 10), teal-blue on the Seq2 (right) side.
   Fixed palette values so the LED never colour-cycles.
   (If your knob LEDs are white-only, set TRIG_MIX_TWO_COLOUR=false above.) */
const TRIG_MIX_CENTRE = 10;                /* ==>> EDIT ME: centre band half-width */
function trigMixLed(){
  const t=U.trig_mix;
  if(TRIG_MIX_TWO_COLOUR){
    if(t < -TRIG_MIX_CENTRE) return LED_RED;    /* left / Seq1 */
    if(t >  TRIG_MIX_CENTRE) return LED_BLUE;   /* right / Seq2 */
    return LED_WHITE;                            /* centre +/-10 */
  }
  /* white-only fallback: off at centre, steady white when engaged either side */
  return (t<-TRIG_MIX_CENTRE||t>TRIG_MIX_CENTRE) ? LED_WHITE : LED_OFF;
}

function desiredLEDs(){
  const list = [];
  /* step buttons: SEQ1 red bit / SEQ2 blue bit; both heads yellow (held on stop) */
  for(let i=0;i<8;i++){
    let c = U.s1_bits[i]?LED_RED:LED_OFF;
    if(i===U.s1_play && i<U.s1_length) c=LED_HEAD;
    list.push(["s", NOTE_STEP_BASE+i, i<U.s1_length? c : LED_OFF]);
  }
  for(let i=0;i<8;i++){
    let c = U.s2_bits[i]?LED_BLUE:LED_OFF;
    if(i===U.s2_play && i<U.s2_length) c=LED_HEAD;
    list.push(["s", NOTE_STEP_BASE+8+i, i<U.s2_length? c : LED_OFF]);
  }
  /* bottom control row */
  const bottom=[LED_GREEN,LED_GREEN,LED_RED,LED_OFF,LED_GREEN,LED_GREEN,LED_RED,LED_OFF];
  for(let col=0;col<8;col++) list.push(["p", padNote(0,col), bottom[col]]);
  /* keyboard rows 1-3: octave roots white, active pad teal, other blue */
  for(let row=1;row<=3;row++)
    for(let col=0;col<8;col++){
      let c=LED_BLUE;
      if(((padSemis(row,col)%12)+12)%12===0) c=LED_WHITE;
      if(row===U.padRow && col===U.padCol) c=LED_TEAL;
      list.push(["p", padNote(row,col), c]);
    }
  /* knob indicator LEDs (CC 71-78): RGB palette (not brightness). Colour-code
     the knobs per page so groups read at a glance; free knobs are off; Trig Mix
     is the 3-colour balance. (Screen still shows the proportional value bar.)
     ==>> EDIT ME: change these palette values to recolour the knob groups. */
  const KNOB_COLS = (U.page===0)
    ? [LED_RED, LED_RED, LED_RED, null,     LED_BLUE, LED_BLUE, LED_BLUE, LED_OFF]   /* SEQUENCERS: k4=trigmix */
    : [LED_WHITE,LED_WHITE,LED_WHITE,LED_WHITE, LED_GREEN,LED_GREEN, LED_OFF, LED_OFF]; /* GLOBAL */
  for(let k=0;k<8;k++){
    const cc=CC_KNOB_BASE+k;
    let col;
    if(U.page===0 && k===3){ col=trigMixLed(); }        /* Trig Mix stays 3-colour */
    else if(U.page===0 && k===7){ col=(U.g_reset<RESET_LABELS.length-1)?LED_ORANGE:LED_OFF; }  /* Reset Both: lit when armed */
    else col = (KNOB_COLS[k]===null? LED_OFF : KNOB_COLS[k]);
    list.push(["b", cc, col]);
  }
  /* other buttons */
  list.push(["b",CC_UP,LED_GREY]);
  list.push(["b",CC_DOWN,LED_GREY]);
  list.push(["b",CC_SHIFT,LED_WHITE]);
  list.push(["b",CC_BACK,LED_WHITE]);
  return list;
}

function refreshLeds(){
  if((pollTick%120)===0){ for(const k in shownLED) delete shownLED[k]; }
  const want = desiredLEDs();
  let budget = 14;
  for(let n=0;n<want.length;n++){
    const kind=want[n][0], addr=want[n][1], color=want[n][2];
    const key=kind+addr;
    if(shownLED[key]===color) continue;
    if(budget<=0) break;
    if(kind==="b") sharedSetButtonLED(addr, color&0x7F, true);
    else           sharedSetLED(addr, color&0x7F, true);
    shownLED[key]=color;
    budget--;
  }
}

/* ---- display: 8 knob cells laid out like the physical encoders ---- */
const CELL_W = 32;
function drawKnobCell(idx, label, valStr, frac){
  if(!label) return;
  const col = idx % 4, row = Math.floor(idx / 4);
  const x = col * CELL_W;
  const yTop = 12 + row * 26;
  print(x + 2, yTop, label, 1);
  if(frac !== null && typeof draw_rect === "function"){
    const BW = CELL_W - 6;
    draw_rect(x + 2, yTop + 9, BW, 5, 1);
    const fw = Math.max(0, Math.min(BW - 2, Math.round((BW - 2) * frac)));
    if(fw > 0 && typeof fill_rect === "function") fill_rect(x + 3, yTop + 10, fw, 3, 1);
  }
  print(x + 2, yTop + 16, valStr, 1);
}
function xposeStr(){ return (U.pad_semis>0?"+":"")+U.pad_semis; }

function draw(){
  if(typeof clear_screen!=="function") return;
  clear_screen();
  if(U.page===0){
    print(0,0,"SEQUENCERS   Xpose "+xposeStr(),1);
    drawKnobCell(0,"1Cor", ""+U.s1_corrupt,            U.s1_corrupt/100);
    drawKnobCell(1,"1Rng", ""+U.s1_cv_range,           U.s1_cv_range/100);
    drawKnobCell(2,"1Len", ""+U.s1_length,             (U.s1_length-1)/7);
    drawKnobCell(3,"Mix",  (U.trig_mix>0?"+":"")+U.trig_mix, (U.trig_mix+63)/127);
    drawKnobCell(4,"2Cor", ""+U.s2_corrupt,            U.s2_corrupt/100);
    drawKnobCell(5,"2Rng", ""+U.s2_cv_range,           U.s2_cv_range/100);
    drawKnobCell(6,"2Len", ""+U.s2_length,             (U.s2_length-1)/7);
    drawKnobCell(7,"Rst", RESET_LABELS[U.g_reset],     U.g_reset/(RESET_LABELS.length-1));
  } else {
    print(0,0,"GLOBAL       Xpose "+xposeStr(),1);
    drawKnobCell(0,"Scale", SCALES[U.scale].name.substr(0,5), U.scale/(SCALES.length-1));
    drawKnobCell(1,"Key",   NOTE_NAMES[((U.key%12)+12)%12],   U.key/11);
    drawKnobCell(2,"Rate",  RATE_NAMES[U.rate],               U.rate/(RATE_NAMES.length-1));
    drawKnobCell(3,"NoteLn",GATE_NAMES[U.gate],               U.gate/(GATE_NAMES.length-1));
    drawKnobCell(4,"S1 Ch", ""+(U.s1_channel+1),              U.s1_channel/15);
    drawKnobCell(5,"S2 Ch", ""+(U.s2_channel+1),              U.s2_channel/15);
    drawKnobCell(6,"", "", null);
    drawKnobCell(7,"", "", null);
  }
}

/* ---- lifecycle ---- */
globalThis.init = function(){
  assertOwnedParams();
  setP("suspend","0");
};
let saveTick=0;
globalThis.tick = function(){
  pollDsp();
  if((pollTick % 30)===0) assertOwnedParams();
  refreshLeds();
  draw();
  if((++saveTick % 120)===0) setP("save","1");   // autosave ~2s
};
globalThis.onMidiMessageExternal = function(data){ /* external notes not used for key */ };
globalThis.onUnload = function(){ setP("panic","1"); };
