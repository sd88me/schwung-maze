# Maze — Moog-Labyrinth-style modules for Ableton Move (Schwung)

Moog-Labyrinth-inspired modules for the
[Schwung](https://github.com/charlesvestal/schwung) framework on Ableton Move.
This repository ships **three** modules:

| Module | ID | Type | What it is |
|---|---|---|---|
| **Maze** | `maze_seq` | overtake tool | Full pad + step-button instrument with its own display |
| **Maze Lite** | `maze_seq_lite` | slot MIDI FX | The same engine as a chain MIDI-FX slot (auto knob menu) |
| **Maze Voice** | `maze-voice` | sound generator | Monophonic thru-zero oscillator / wavefolder / SVF voice — the Labyrinth's synth section as a chain slot |

`maze_seq` / `maze_seq_lite` run two 8-step generative sequencers that clock-sync
to the Move transport, quantise random voltages to a scale, and play MIDI out — a
recreation of the Labyrinth's dual generative sequencer section. `maze-voice` is
the matching monosynth: pair it in a Signal Chain slot behind Maze Lite.

---

## Features

- **Dual 8-step generative sequencers** with per-step random CV, quantised to a scale.
- **Corrupt (0–100)** — mutates stored voltages; past 12 o'clock also flips bits (evolving patterns).
- **Range (0–100)** — bipolar pitch spread around the root.
- **Trig Mix** — velocity cross-fade between Seq 1 and Seq 2 (−63 = Seq1 only @127, centre = both @100, +64 = Seq2 only @127).
- **Length / Bit Flip / Advance** per sequencer.
- **12 scales**, selectable key, note-rate (1/32…1 bar), gate length (1/4…2 steps).
- **Clock-synced** to the Move transport (24 PPQN, start/stop/continue).
- **Pattern + position persistence** (Maze tool) — survives exit.

### Maze (tool) hardware layout
- **16 step buttons** = the two sequencers.
  1–8 = Seq1 (red bit / yellow play head),
  9–16 = Seq2 (blue bit / yellow play head).
  Press to flip a bit. The play head stays lit when the transport is stopped.
- **Top 3 pad rows** = a fixed scale keyboard (each row = one octave, ascending
  scale degrees left→right; the middle-row first pad is the root). Press a pad to
  transpose the whole sequence to that interval. Octave roots light white, the
  active pad lights green.
- **+ / −** = shift the pad keyboard up/down an octave to reach higher/lower
  registers. The sounding note doesn't change — the layout moves, and the
  highlighted pad follows the same note to its new row.
- **Bottom pad row** = per-sequencer advance ◄/► (orange) and length−minus1 (red).
- **Jog wheel** = switch between the two knob pages.
- **Knobs — Sequencers page:**
  1 - Corrupt, 2 - Range, 3 - Length (Seq1, red LEDs);
  4 - Trig Mix (red/white/blue LED);
  5 - Corrupt, 6 - Range, 7 - Length (Seq2, blue LEDs).
- **Knobs — Global page:** 1 Scale, 2 Key, 3 Note Rate, 4 Note Length (white LEDs);
  5 - Seq1 MIDI Channel, 6 - MIDI Seq2 Channel (green LEDs).
- **Back** = hide (keeps playing in the background); **Shift+Back** = exit.

### Maze Lite (slot MIDI FX)
Insert in a MIDI-FX slot, route to a synth, press Play. Pages in order:
- **Sequencer 1:** Corrupt, Range, Length, Bit Flip, Advance, Trig Mix.
- **Sequencer 2:** Corrupt, Range, Length, Bit Flip, Advance, Trig Mix (same
  Trig Mix as page 1, kept in sync).
- **Global:** Scale, Note Rate, Note Length.

Bit Flip and Advance are momentary buttons (fire once per press/turn). Incoming
notes set the root (transpose).

### Maze Voice (chainable sound generator)
A monophonic thru-zero-FM voice: sine VCO + triangle modulator with TZFM, a
wavefolder and a state-variable filter (LP→BP morph) in switchable routing
(`VCW>VCF` / parallel / `VCF>VCW`), per-channel warm mixer overdrive, a
Boss-style output drive, and two decay envelopes. Knob-grid pages: **Voice,
WaveFolder, Filter, Tone, Mod Routing, Randomise**. The Randomise page has four
latching toggles (Voice / WaveFolder / Filter / Tone) that arm which pages the
momentary **Generate** button randomises; the toggles save with the preset.

---

## Install

### Option A — manual (for testers, no store needed)
1. Download the latest tarballs from [Releases](../../releases).
2. Extract onto the Move (each module goes in its own category dir):
   ```bash
   # tool
   scp maze_seq-module.tar.gz ableton@<MOVE_IP>:/tmp/
   ssh ableton@<MOVE_IP> 'cd /data/UserData/schwung/modules/tools && tar xzf /tmp/maze_seq-module.tar.gz'
   # lite (slot MIDI FX)
   scp maze_seq_lite-module.tar.gz ableton@<MOVE_IP>:/tmp/
   ssh ableton@<MOVE_IP> 'cd /data/UserData/schwung/modules/midi_fx && tar xzf /tmp/maze_seq_lite-module.tar.gz'
   # voice (chainable sound generator)
   scp maze-voice-module.tar.gz ableton@<MOVE_IP>:/tmp/
   ssh ableton@<MOVE_IP> 'cd /data/UserData/schwung/modules/sound_generators && tar xzf /tmp/maze-voice-module.tar.gz'
   ```
3. Power-cycle the Move. **Maze** appears in Shadow → Tools; **Maze Lite** in a
   MIDI-FX slot; **Maze Voice** in a sound-generator slot.

### Option B — Schwung Manager / Module Store
Once listed in the Schwung catalog, install from the Schwung Manager web UI at
`http://move.local:7700`.

---

## Build from source

Requires Docker (the build cross-compiles the DSP for the Move's ARM64 chip).

```bash
bash scripts/build.sh
# produces:
#   dist/maze_seq-module.tar.gz
#   dist/maze_seq_lite-module.tar.gz
#   dist/maze-voice-module.tar.gz
```

### Vendored headers
The DSP compiles against two Schwung API headers. Copy them from a Schwung
checkout into `src/include/` before building (kept out of git so they stay in
sync with the host ABI):

```bash
cp <schwung>/src/host/plugin_api_v1.h    src/include/
cp <schwung>/src/host/midi_fx_api_v1.h   src/include/
```

---

## Repository layout

```
src/
  maze_seq/                 # overtake tool
    module.json  ui.js  help.json
    dsp/maze_seq.c
  maze_seq_lite/            # slot MIDI FX
    module.json  help.json
    dsp/maze_seq_lite.c
  maze-voice/               # chainable sound generator
    module.json  help.json
    dsp/maze_voice.c
  include/                  # vendored Schwung headers (not committed)
scripts/
  build.sh  Dockerfile
.github/workflows/release.yml
release.json  catalog-entries.json
```

---

## Publishing (tagged releases)

```bash
# bump versions in the src/*/module.json files, commit, then:
git tag v1.0.0
git push --tags
```

GitHub Actions cross-compiles all modules, attaches the tarballs to the
release, and updates `release.json` on `main`. To be listed in the Schwung
Module Store, open a PR adding the entries in `catalog-entries.json` to
[`module-catalog.json`](https://github.com/charlesvestal/schwung/blob/main/module-catalog.json).

---

## Credits & license

Created by Sam Di Domizio. Inspired by the dual generative sequencer of the
Moog Labyrinth (concept only; original code). Built for
[Schwung](https://github.com/charlesvestal/schwung) by Charles Vestal.

Licensed under the MIT License — see [LICENSE](LICENSE).
