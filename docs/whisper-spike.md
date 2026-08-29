# Whisper tap spike (#19)

The question: can the Pi 5 run whisper.cpp speech recognition alongside
the 1080p60 passthrough? The spike proved it can, and the tap grew into
a managed feature: whisper.cpp is part of the standard build, the tap
is toggled live, and models are selected and downloaded through the web
interface. `docs/rest-api.md` documents the endpoints; this file is the
record of what the spike found. English-only, transcripts only logged —
nothing is rendered, matched, or synced (that is #10/#21/#433
territory).

**Pi verdict (2026-08-27, appliance run):** whisper keeps up with the
passthrough. Transcription quality with tiny.en is poor as expected —
the hypothesis is that it is still good enough for a one-shot auto-sync
action (#433), which is what the matcher's trigram voting is designed
to tolerate.

## What shipped

- `SUBTITLER_ENABLE_WHISPER` (default ON) fetches whisper.cpp v1.9.3;
  OFF builds a stub transcriber and `--whisper` just fails model
  loading. No separate whisper binary or build.
- The capture audio chain carries `tee name=whisper_tee` whenever the
  audio branch exists; the tap branch (leaky queue →
  audioconvert/audioresample → 16 kHz mono F32LE → dropping appsink)
  starts gated, so a disabled tap costs a tee and a queue, nothing
  more.
- `Stream::SetWhisperState(enabled, model-path)` toggles live — no
  pipeline rebuild: it swaps the transcriber (an `atomic<shared_ptr>`
  the whisper thread copies per window) and flips the branch's
  `DropGate`.
- Models are ggml files in `<state-dir>/models/`, never
  auto-downloaded: the web UI fetches them from HuggingFace in the
  browser (CORS is allowed on both the resolve redirect and the CDN)
  and stores them via `PUT /api/whisper/models/<name>`; selection and
  the on/off toggle ride `GET`/`PUT /api/whisper` and persist in
  `[whisper] enabled`/`model`. `--whisper=<path>` overrides for a run.
- Transcripts log at `stream:info`; per-window inference time at
  `stream:debug`.

## What the spike (and the hardening after it) found

**A tee without per-branch queues deadlocks a live pipeline.** The two
sinks' preroll waits serialize through the tee and the source never
reaches PLAYING. Both capture-tee branches carry their own queue.

**A gated branch needs `async=false` on its sink.** Found when the tap
became runtime-toggleable: with the gate closed at startup, the
starving whisper appsink held the whole pipeline in PAUSED, passthrough
included — the same rule the preview branch already follows, for the
same reason.

**gst-launch is not a valid oracle for either stall.** It force-PLAYINGs
live pipelines on stream-start and sails past both. The repros that
isolated these were minimal appsink-pull programs; the
regression tests live in `tests/integration_tests.cpp` (`whisper tap
flows converted audio`, `whisper gate drops the branch while closed`).

Verified before any hardware run: the tap delivers exactly 16 kHz mono
F32LE (live test with audiotestsrc standing in for the CV105), and real
transcription works through the real code path — whisper.cpp's
`samples/jfk.wav` fed in 10 ms chunks comes out as "And so my fellow
Americans, ask not what your country can do for you …" (model-gated
`tests/whisper_tests.cpp`). Baseline timing on the 16-core dev machine:
a 5 s window transcribes in ~260–320 ms with tiny.en at 2 threads.

## Remaining spike shortcuts, to revisit as #19's follow-ups land

- Threads, window size, and language are compile-time constants in
  `src/stream/whisper_transcriber.cpp`.
- Teardown can block on one window's inference; `whisper_full` has no
  cancellation point.
- Model uploads buffer the whole file in memory once (libsoup reads the
  request body before the handler runs); the 512 MiB cap bounds it.
- Window timestamps: the transcriber doesn't carry buffer PTS through
  yet — auto-sync (#433) needs that to map a matched cue back onto the
  film clock.
