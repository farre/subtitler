#!/usr/bin/env bash
# Runs the whisper tap spike (#19) on the Pi 5 appliance: builds with
# SUBTITLER_ENABLE_WHISPER=ON, fetches a ggml model, and runs the real
# passthrough with whisper logging on. Lives on the whisper-spike
# branch — check that out first.
#
# Usage: tools/whisper-spike.sh [model-path]
#   model-path defaults to ~/ggml-tiny.en.bin (downloaded when missing).
#   A custom path must already exist — its model is not downloaded.
#
# Watch for "Whisper transcribed a 5 s window in X": sustained X well
# under 5 s with zero dropped frames on exit means the Pi keeps up with
# whisper alongside the passthrough. Temperature and throttle state
# print before the run and on exit (Ctrl-C) for comparison; throttled=0x0
# is the clean state.
set -euo pipefail

build_dir=build/whisper
model=${1:-"$HOME/ggml-tiny.en.bin"}
model_url=https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin

cd "$(dirname "$0")/.."

report_thermals() {
  vcgencmd measure_temp
  vcgencmd get_throttled
}

trap report_thermals EXIT

cmake -B "$build_dir" -GNinja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DSUBTITLER_ENABLE_WHISPER=ON
cmake --build "$build_dir"

if [[ ! -f "$model" ]]; then
  if (($# > 0)); then
    echo "Model not found: $model" >&2
    exit 1
  fi
  curl -sSL -o "$model" "$model_url"
fi

report_thermals

echo "Transcripts and per-window inference times follow; Ctrl-C to stop."
SUBTITLER_LOG=stream:debug "$build_dir/subtitler" --whisper="$model"
