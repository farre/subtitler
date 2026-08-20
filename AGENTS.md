# AGENTS.md

## What this is

Headless HDMI capture appliance (C++26 + GStreamer): grabs 1080p60 video from a
V4L2 device and outputs it via DRM/KMS. Implemented: the low-latency
passthrough plus a pink no-signal fallback (when the capture side stops, the
frame buffer is kept stocked with generated pink frames and output keeps
running), the MJPEG preview from #376 (`--web` serves `/api/preview.jpg` and
`/api/preview.mjpeg` on port 8080), the SRT subtitle overlay from #438
(`--subtitles=<file>`, plus boot resume of the state dir's `active` marker),
and the subtitle upload endpoint from #212 (`PUT /api/subtitles/<title>`
stores into the library, marks it active, and switches it in live; unmatched
routes fall back to static files from the web root). Planned but NOT yet
implemented: the #15 web interface proper (static assets in `web/`, which
today holds only a placeholder `index.html`), web sync controls, config file
loading, systemd unit (`config/subtitler.example.json` and
`config/subtidlerd.service` are both empty placeholders).

## GitHub milestones

The roadmap lives in six GitHub milestones named `N. Title` (issue #1 was
split into issues #2–#29 across them; those have their own sub-issues,
numbered into the #80s). The numeric prefix defines execution order — GitHub
has no milestone ordering — so keep prefixes sequential when adding or
inserting milestones.

## Technology

C++26 is allowed! We use CMake and ninja.

## Philosophy

We follow YAGNI and IWYU. If possible also TDD.

## Build

```sh
cmake --preset default
cmake --build --preset default
```

- The `default` preset in `CMakePresets.json` sets the Ninja generator, `build/` binary dir, and clang++ — the README, the VSCode task, and manual builds all go through it.
- If `build/` already exists configured with Makefiles, delete it first — CMake errors on generator mismatch instead of switching in place.
- Requires CMake >= 3.30 and a very recent C++26 compiler (uses `std::print`, `std::format`, `std::out_ptr`, `std::jthread`). Verified with Clang 22.1.8 (dev machine) and clang 19 on the Pi 5 target (trixie; see `docs/pi-setup.md`).
- Dependencies resolve via pkg-config (all `REQUIRED`; configure fails if dev packages are missing): gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0, gstreamer-video-1.0, glib-2.0 / gobject-2.0 / gio-2.0, libsoup-3.0, libdrm (`subtitler-probe` only) and alsa (probe plus the stream lib's audio-output probing). Tests additionally use doctest via `find_package(doctest REQUIRED)` (CMake package, not pkg-config).
- Tests run with `ctest --test-dir build` (doctest unit tests under `tests/`, registered via `doctest_discover_tests`; gated on `BUILD_TESTING`, default ON). The description tests parse real pipelines with `gst_parse_launch`, so the machine running the suite needs the GStreamer runtime plugins (v4l2src, appsink/appsrc, kmssink) installed, not just the dev packages; the audio-branch constructibility check additionally uses alsasrc but skips when it is absent. The throughput test (`tests/integration_tests.cpp`) runs a real `Stream` for ~8 s against the capture device (default `/dev/video0`, override with `SUBTITLER_TEST_VIDEO_DEVICE`) with the audio output on the ALSA `null` PCM device — it guards the shared-timeline and latency-recalculation path (#128, #437) and skips when no usable CV105 is present; the `null` device works only for playback, never as a capture source (it is not rate-limited and poisons the capture pipeline's clock). No CI, no lint targets. Verification = a clean `cmake --build build` plus passing ctest.
- Formatting: clang-format with the repo's `.clang-format` (Google base, 2-space indent, 80 columns).

## Layout and conventions

- Naming follows the Google C++ Style Guide: types and functions are `CamelCase`, constants are `kCamelCase`, variables are `snake_case` with a trailing `_` on class data members. snake_case is kept only where a name deliberately mirrors an external API (the `gst_unref` overloads in `deleters.h`, `Fd::get`/`release`/`reset`, `ResetGuard::release`).
- All headers are private and live next to their sources under `src/`. The include root is `src/`, so includes look like `#include "stream/stream.h"`. The root `include/` directory is intentionally empty, reserved for a future public API — do not put headers there.
- `src/stream/` builds the static lib `stream` (alias `subtitler::stream`), consumed only by the `subtitler` executable from `src/main.cpp`. New modules should follow the same `src/<module>/` pattern with their own CMakeLists.
- `src/probe/` builds the `subtitler-probe` diagnostic executable (#94): capability probes via native APIs (GStreamer registry, V4L2 ioctls, libdrm, ALSA), a pipeline recommendation/negotiation check, text and `--json` output. Probe is standalone and contained: it may include from `utils/` and `stream/`, but nothing outside `src/probe/` includes from it.
- `src/web/` builds the static lib `web` (alias `subtitler::web`): the appliance web server (`WebServer`, libsoup 3) with the MJPEG preview endpoints (#382), the subtitle upload endpoint `PUT /api/subtitles/<title>` (#212, storage/activation injected as a handler by main.cpp), and a static-file fallback for unmatched routes (allowlist `.html`/`.js`/`.css`/`.png` — the allowlist is the MIME map; `/` maps to `index.html`, traversal and non-GET are 404). It reads the shared preview frame buffer and toggles the preview gate via an activation callback; it must not depend on `stream/` or Gst.
- `web/` holds the static web assets served by the fallback (#212) — the future #15 UI; today just a placeholder `index.html`. Installed to `<prefix>/share/subtitler/web`; resolved at runtime as the first `<data-dir>/subtitler/web` across `$XDG_DATA_HOME`/`$XDG_DATA_DIRS`, or via `--web-root=<dir>` in development and tests.
- `src/utils/` is a header-only INTERFACE lib `utils` (alias `subtitler::utils`) with shared utilities: `reset_guard.h` (`ResetGuard`), `unique_ptr.h` (`FunctionDeleter`/`UniquePtr`, RAII for C APIs that release via a free function), `preview_frame.h` (`PreviewFrameBuffer`, the shared latest-JPEG buffer between the stream and web modules), and `paths.h` (XDG state-dir and web-root data-dir resolution, the sharded subtitle library — entries stored as `subtitles/<bucket>/<title>` with the bucket = the title's first letter lowercased or `_`, `StoreSubtitle`, and `active`-marker subtitle resume accepting both sharded and legacy flat names, #212/#438). Utils must stay dependency-free — no Gst, no other modules. Anything Gst-dependent belongs in `stream/` (e.g. `deleters.h`); anything used by only one module stays local to that module.
- `src/stream/stream.{h,cpp}` holds the `Stream` class (pimpl): it owns the capture/output pipelines, the worker threads (video capture, audio capture, and output; audio runs only when the audio branch is enabled), and the `FrameBuffer`s (one video, one audio). Pipeline description strings are free functions in the same files (tested by `tests/description_tests.cpp`); the capture pipeline description composes separate video and audio branch descriptions. `main.cpp` is only argument parsing, signal handling, and the poll loop. When capture dies (EOS/error), `Stream::Poll` tears the capture pipeline down and the capture-side thread switches to feeding pink no-signal frames — the output side just renders whatever the frame buffer contains. `RestartCapture`/`RestartOutput` are restart-safe entry points reserved for the future web UI; nothing restarts automatically, so there are no restart loops.
- `src/stream/` also has `frame_buffer.*` (the app-owned frame queue between the threads), `preview_gate.*` (the pad-probe gate for the MJPEG preview branch, #379), and `deleters.h`.
- `tests/` holds the doctest unit tests. Libs expose nothing publicly, so test targets set their own `target_include_directories` for `src/`.
- `docs/` holds appliance documentation (e.g. `docs/pi-setup.md`, the verified Pi 5 + CV105 hardware profile). The README stays the how-to; docs/ is the record — don't duplicate commands between them.
- `tools/` holds development/measurement utilities (`generate-sync-test-video.sh`, the #133 flash-and-click clip generator; `lorem-ipsum.srt`, 100 filler cues over ~10 minutes for trying the #212 upload endpoint and subtitle rendering). Not part of the build.
- `cmake/CompilerWarnings.cmake` is an **empty placeholder**; warning flags (`-Wall -Wextra -Wpedantic`) are set directly on targets in the root `CMakeLists.txt`. Don't grep the module file for warning config.
- `cmake/Sanitizers.cmake` holds the optional sanitizers: `SUBTITLER_ENABLE_ASAN` / `SUBTITLER_ENABLE_UBSAN` (both default OFF), applied directory-scope so tests are instrumented too. Build them via the `asan-ubsan` configure/build/test presets (binary dir `build/asan-ubsan`).
- `cmake/Dependencies.cmake` declares GLib and Threads that nothing links yet — intentional. libsoup is used by `src/web/`.
- Wrap GStreamer objects with the RAII deleters in `src/stream/deleters.h` (see the `GstPointer` aliases used throughout `src/stream/stream.cpp`) instead of raw `gst_*_unref` calls. Non-owning references (GObject casts, transfer-none getters) use `GstView<T>` — never wrap those in an owning `GstPointer`; the double-unref this causes was #134.
- Installation covers the `subtitler` and `subtitler-probe` binaries (`install(TARGETS ...)` + GNUInstallDirs in the root CMakeLists) and the `web/` assets (`install(DIRECTORY web ...)` to `${CMAKE_INSTALL_DATADIR}/subtitler`). When `config/subtidlerd.service` and `config/subtitler.example.json` get implemented, add install rules for them too.

## Gotchas

- Resolution/format constants (1080p60 YUY2) and the CV105 audio constants (ALSA device `hw:CARD=Video,DEV=0`, S16LE/48kHz/stereo) live once in `src/stream/stream.cpp`; `src/probe/pipeline.cpp` keeps its own copies of the video ones for its standalone negotiation checks. They may move to the config file once that exists.
- The KMS output pipelines hardcode `kmssink driver-name=vc4` (Raspberry Pi). Output mode is selected with `--output=software|pisp|window|null` (`OutputMode` in `src/stream/stream.h`); the `kms` modes need a real V4L2 capture device and a KMS display — don't use them as a smoke test on a dev machine; build-only verification is the norm. `window` (glimagesink) and `null` (fakesink) are the dev-machine modes; `--no-audio` drops the CV105 audio branch for machines without it. The `pisp` mode is currently blocked upstream — pispconvert renders NV12 output blue on BCM2712C1 (raspberrypi/libpisp#76, see docs/pi-setup.md); `software` is the default.
- No vc4 plane supports packed 4:2:2 (no YUYV/UYVY/YVYU/VYUY — verified via `modetest -p`), so captured YUY2 frames **cannot be scanned out directly**: a conversion step (e.g. YUY2→NV16) before kmssink is mandatory. The empirical gst-launch confirmation pair is still pending; hardware profile in `docs/pi-setup.md`.
- Capture and output share one timeline (#437): `ConfigureTimeline` pins the system clock (still load-bearing — without it the pipeline adopts the alsasink ring-buffer clock, which starts at zero when the device starts, #128) and sets a common base time with start-time management left to the application, so captured PTS are valid output timestamps and capture restarts stay monotonic. Buffers are pushed with their capture PTS plus the `--audio-offset` counter-stream delay; there is no re-anchoring (the old `OutputAnchor` discarded the capture-timestamp A/V relationship — the unprincipled ~150 ms skew that needed `--audio-offset=-150`). Latency is **not** pinned: GStreamer computes it automatically, `PollBus` drains the bus and calls `gst_bin_recalculate_latency` on `GST_MESSAGE_LATENCY`, the output appsrcs report the capture-side latency hidden behind the appsink/appsrc boundary via `min-latency`/`max-latency`, and the alsasink runs a small ring buffer (`buffer-time=40000` µs — a default alsasink reports ~200 ms, which no low latency target can honor). In-flight buffering is time-sized (appsrc `max-time` 300 ms) so the render schedule can't starve the sources — that starvation was why latency was pinned in the first place (#128). The screensaver timestamps in the shared running time, capture teardown flushes both frame buffers, and the first buffer after a flush carries `DISCONT`. The `--audio-offset` trim for the remaining physical offsets (CV105 internal differential, TV audio path) measured **zero** on hardware — real-content playback has no perceptible lag at the default 0 (#437 stage 3, verified on Pi 5 + CV105); the #133 flash-and-click tooling remains for quantitative checks.
- Usage: `subtitler [video-device] [connector-id] [--output=software|pisp|window|null] [--no-audio] [--audio-output-device=<alsa-device>] [--audio-offset=<ms>] [--subtitles=<srt-file>] [--web] [--web-root=<dir>]` (`--audio-offset` shifts audio relative to video: positive delays audio, negative advances it — realized as a PTS shift on the counter-stream so no render deadline falls before buffer arrival; the #130 sync knob, now the trim for measured physical delay, #437). Defaults: `/dev/video0`, auto connector, software NV16 output, CV105 audio capture on, vc4-hdmi audio output auto-detected as the first openable vc4hdmi port via `plughw:` — raw `hw:` can't work, the MAI DAI accepts only IEC958 subframes, see docs/pi-setup.md). `--web` starts the web server on port 8080 and builds the MJPEG preview branch.
- MJPEG web preview (#376, design in docs/video-output.md): the output appsrc feeds a tee where the subtitle overlay will sit; the preview branch is `queue(leaky=1) ! videoscale 640x360 ! jpegenc ! appsink`, gated by a pad probe (`InstallPreviewGate`) that drops all buffers while no web client is connected and keeps every 6th frame (60→10 fps) while active. Non-obvious, all regression-tested in `tests/output_pipeline_tests.cpp`: a `valve` can't gate the branch (it fails serialized queries while dropping, stalling pipeline-wide latency computation); the preview appsink needs `async=false` (a starving gated branch would otherwise hold the whole pipeline in preroll, HDMI included — the output pipeline isn't live in the preroll sense); every tee branch needs its own queue (the HDMI one non-leaky, sized 1); and `videorate` is unusable here (it anchors its cadence to the segment start and emits a catch-up burst after every gated gap). Encoding runs only while an MJPEG client is connected; `preview.jpg`/`preview.mjpeg` serve a magenta placeholder when nothing has been encoded yet (seeded at startup, same BT.601 values as the no-signal screen).
- SRT subtitles (#438): when a path is configured, the output pipeline gains `subtitleoverlay` (which auto-plugs a textoverlay renderer) between `output_source` and the tee, plus a subtitle branch `filesrc ! application/x-subtitle ! subparse name=subtitle_parser ! subtitle_overlay.subtitle_sink`. subparse is plugged **explicitly** because subtitleoverlay's parser autoplugging can't be relied on to match it, and the named parser is where the anchor lives: cue time t renders at `anchor + delay + t` via `gst_pad_set_offset` on the parser's src pad (anchor = output running time at `StartOutput`, delay = the live trim from `Stream::SetSubtitleDelay`; verified end-to-end by the pixel-checking `output pipeline subtitle branch` test). `Stream::SetSubtitleFile` switches or detaches the SRT through the restart-safe output rebuild (capture never restarts); `SetSubtitlesVisible` is the live `silent` toggle. SRTs live in `$XDG_STATE_HOME/subtitler/` (library in `subtitles/<bucket>/` sharded by the title's first letter, `_` for non-letters, #212; an `active` marker for boot resume). Rendering needs fonts installed — pango silently composites nothing without them (docs/pi-setup.md). The tee comment above is now historical: the tee sits right after the overlay, so the MJPEG preview shows composited subtitles.

## OpenCode config

`opencode.json` pre-approves `cmake`/`ninja`/`ctest`, read-only git
(`status`/`diff`/`log`/`show`), `ls`, `rg`, and `gh issue view`; every other
bash command prompts. That's expected — don't work around the prompts.
