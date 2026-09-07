#!/usr/bin/env bash
# =============================================================================
# Build the Maze modules for the Ableton Move (ARM64) and package them
# into installable tarballs under dist/.
#
#   dist/maze_seq-module.tar.gz        (folder: maze_seq/)
#   dist/maze_seq_lite-module.tar.gz   (folder: maze_seq_lite/)
#   dist/maze-voice-module.tar.gz      (folder: maze-voice/)
#
# Requires Docker. Cross-compiles the DSP with aarch64-linux-gnu-gcc.
# Before building, vendor the two Schwung headers into src/include/:
#   cp <schwung>/src/host/plugin_api_v1.h  src/include/
#   cp <schwung>/src/host/midi_fx_api_v1.h src/include/
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=maze-seq-builder

# Sanity: headers must be present (kept out of git; see README).
if [[ ! -f src/include/plugin_api_v1.h || ! -f src/include/midi_fx_api_v1.h ]]; then
  echo "ERROR: missing vendored headers in src/include/" >&2
  echo "  copy plugin_api_v1.h and midi_fx_api_v1.h from a Schwung checkout's src/host/" >&2
  exit 1
fi

echo "== Building toolchain image =="
docker build -t "$IMG" scripts

echo "== Cross-compiling + packaging =="
docker run --rm -v "$PWD":/build -w /build "$IMG" bash -euxc '
  CROSS=aarch64-linux-gnu-
  rm -rf dist && mkdir -p dist

  # ---- maze_seq (overtake tool) ----
  mkdir -p dist/maze_seq
  ${CROSS}gcc -shared -fPIC -O2 -Isrc/include \
      src/maze_seq/dsp/maze_seq.c \
      -o dist/maze_seq/dsp.so -lm -lpthread
  cp src/maze_seq/module.json dist/maze_seq/
  cp src/maze_seq/ui.js       dist/maze_seq/
  cp src/maze_seq/help.json   dist/maze_seq/

  # ---- maze_seq_lite (slot MIDI FX) ----
  mkdir -p dist/maze_seq_lite
  ${CROSS}gcc -shared -fPIC -O2 -Isrc/include \
      src/maze_seq_lite/dsp/maze_seq_lite.c \
      -o dist/maze_seq_lite/dsp.so -lm
  cp src/maze_seq_lite/module.json dist/maze_seq_lite/
  cp src/maze_seq_lite/help.json   dist/maze_seq_lite/

  # ---- maze-voice (chainable sound generator, plugin_api_v2) ----
  mkdir -p dist/maze-voice
  ${CROSS}gcc -shared -fPIC -O2 -ffast-math -fvisibility=hidden -DOVERSAMPLE=2 -Isrc/include \
      src/maze-voice/dsp/maze_voice.c \
      -o dist/maze-voice/dsp.so -lm
  ${CROSS}nm -D dist/maze-voice/dsp.so | grep -q move_plugin_init_v2 \
      || { echo "maze-voice: dsp.so is missing move_plugin_init_v2" >&2; exit 1; }
  cp src/maze-voice/module.json dist/maze-voice/
  cp src/maze-voice/help.json   dist/maze-voice/

  # confirm the .so files are really ARM64
  file dist/maze_seq/dsp.so dist/maze_seq_lite/dsp.so dist/maze-voice/dsp.so

  # ---- tarballs (folder name must equal the module id) ----
  ( cd dist && tar -czf maze_seq-module.tar.gz      maze_seq )
  ( cd dist && tar -czf maze_seq_lite-module.tar.gz maze_seq_lite )
  ( cd dist && tar -czf maze-voice-module.tar.gz    maze-voice )
  ls -la dist
'

echo "== Done =="
echo "dist/maze_seq-module.tar.gz"
echo "dist/maze_seq_lite-module.tar.gz"
echo "dist/maze-voice-module.tar.gz"
