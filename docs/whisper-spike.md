# Whisper tap spike (#19)

The question: can the Pi 5 run whisper.cpp speech recognition alongside
the 1080p60 passthrough? This spike wires a tap into the capture audio
branch so we can measure it on the appliance. It lives on the
`whisper-spike` branch, is English-only, and only **logs** what it
hears — nothing is rendered, matched, or persisted. That is #10/#21
territory; this is strictly the feasibility measurement.

## What the spike builds

With `SUBTITLER_ENABLE_WHISPER=ON` (default OFF) CMake fetches
whisper.cpp v1.9.3 and `subtitler --whisper=<ggml-model>` activates the
tap:

- The capture audio chain gains `tee name=whisper_tee`; the tap branch
  is a leaky queue (2 s) → audioconvert/audioresample → 16 kHz mono
  F32LE → a dropping appsink.
- A worker thread drains the appsink into `WhisperTranscriber`
  (`src/stream/whisper_transcriber.*`): 5 s windows, English, 2
  threads, the encoder context sized to the window. With the option
  OFF the transcriber compiles to a stub, so the rest of the tree needs
  no ifdefs.
- Transcripts log at `stream:info` ("Whisper heard: …"); per-window
  inference time logs at `stream:debug` ("Whisper transcribed a 5 s
  window in X") — the number that answers the spike's question.
- Recognition lag is bounded by the leaky queue plus the dropping
  appsink, never by inference time; a slow transcriber drops audio,
  it never accumulates debt against the passthrough.

## What the dev machine already found

**A tee without per-branch queues deadlocks a live pipeline.** The two
sinks' preroll waits serialize through the tee and the source never
reaches PLAYING — the app's 2 s `get_state` at capture start would have
hung on the Pi. gst-launch is not a valid oracle here: it force-PLAYINGs
live pipelines on stream-start and sails right past the stall. Both
capture-tee branches now carry their own queue (same rule the output
tee already follows), regression-tested by `whisper tap flows converted
audio` in `tests/integration_tests.cpp`.

Verified before any hardware run: the tap delivers exactly 16 kHz mono
F32LE (live test with audiotestsrc standing in for the CV105), and real
transcription works through the real code path — whisper.cpp's
`samples/jfk.wav` fed in 10 ms chunks comes out as "And so my fellow
Americans, ask not what your country can do for you …" (model-gated
`tests/whisper_tests.cpp`).

Baseline timing on the 16-core dev machine: a 5 s window transcribes
in ~260–320 ms with tiny.en at 2 threads. A sanity floor only — the
Pi 5 measurement is the one that matters.

## Running it on the Pi

```sh
tools/whisper-spike.sh            # ~/ggml-tiny.en.bin, downloaded if missing
tools/whisper-spike.sh <model>    # your own ggml model file
```

The script configures `build/whisper` with the whisper option, builds,
fetches tiny.en if needed, prints temperature/throttle state, and runs
the real passthrough with `SUBTITLER_LOG=stream:debug`. Run it with the
display attached in the default software output mode: `--output=null`
skips the 1080p60 videoconvert and would flatter the CPU budget.

Reading the result:

- **"window in X" sustained well under 5 s, 0 dropped frames** — the
  Pi 5 keeps up. Proceed with #19 for real.
- X creeping toward 5 s, or dropped frames appearing — it doesn't, at
  least not at these settings.
- The script prints `vcgencmd measure_temp` / `get_throttled` before
  and after; `throttled=0x0` must hold or the numbers are suspect.

Knobs if it falls short: `kThreads` 2→4 in
`src/stream/whisper_transcriber.cpp` (2 leaves cores for the
videoconvert; 4 bets the other way), or a quantized tiny
(`ggml-tiny.en-q5_1.bin`, same download URL pattern). If tiny keeps up
with clear headroom, `ggml-base.en.bin` is the next rung for accuracy.

## Spike shortcuts, to revisit if #19 lands for real

- `--whisper` is command-line only; not persisted to the config file.
- whisper.cpp prints its model-load banner to stderr on startup
  (silenceable via `whisper_log_set`).
- Threads, window size, and language are compile-time constants.
- Teardown can block on one window's inference; `whisper_full` has no
  cancellation point.
