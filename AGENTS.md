# AGENTS.md

## What this is

Headless HDMI capture appliance (C++26 + GStreamer): grabs 1080p60 video from a
V4L2 device and outputs it via DRM/KMS. Planned but NOT yet implemented: SRT
subtitle overlay, web controls, config file loading, systemd unit
(`config/subtitler.example.json` and `config/subtidlerd.service` are both
empty placeholders). Current code is the low-latency passthrough only.

## GitHub milestones

The roadmap lives in six GitHub milestones named `N. Title` (issue #1 was
split into issues #2–#29 across them; those have their own sub-issues,
numbered into the #80s). The numeric prefix defines execution order — GitHub
has no milestone ordering — so keep prefixes sequential when adding or
inserting milestones.

## Technology
C++26 is allowed! We use CMake and ninja.

## Build

```sh
cmake --preset default
cmake --build --preset default
```

- The `default` preset in `CMakePresets.json` sets the Ninja generator, `build/` binary dir, and clang++ — the README, the VSCode task, and manual builds all go through it.
- If `build/` already exists configured with Makefiles, delete it first — CMake errors on generator mismatch instead of switching in place.
- Requires CMake >= 3.30 and a very recent C++26 compiler (uses `std::print`, `std::format`, `std::out_ptr`, `std::jthread`). Verified with Clang 22.1.8 (dev machine) and clang 19 on the Pi 5 target (trixie; see `docs/pi-setup.md`).
- Dependencies resolve via pkg-config (all `REQUIRED`; configure fails if dev packages are missing): gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0, gstreamer-video-1.0, glib-2.0 / gobject-2.0 / gio-2.0, libsoup-3.0. Tests additionally use doctest via `find_package(doctest REQUIRED)` (CMake package, not pkg-config).
- Tests run with `ctest --test-dir build` (doctest unit tests under `tests/`, registered via `doctest_discover_tests`; gated on `BUILD_TESTING`, default ON). The description tests parse real pipelines with `gst_parse_launch`, so the machine running the suite needs the GStreamer runtime plugins (v4l2src, appsink/appsrc, kmssink) installed, not just the dev packages. No CI, no lint targets. Verification = a clean `cmake --build build` plus passing ctest.
- Formatting: clang-format with the repo's `.clang-format` (Google base, 2-space indent, 80 columns).

## Layout and conventions

- All headers are private and live next to their sources under `src/`. The include root is `src/`, so includes look like `#include "stream/description.h"`. The root `include/` directory is intentionally empty, reserved for a future public API — do not put headers there.
- `src/stream/` builds the static lib `stream` (alias `subtitler::stream`), consumed only by the `subtitler` executable from `src/main.cpp`. New modules should follow the same `src/<module>/` pattern with their own CMakeLists.
- `src/stream/stream.cpp` and `stream.h` are empty placeholders still listed as lib sources — the lib today is only `description.cpp` + `deleters.h`.
- `tests/` holds the doctest unit tests. Libs expose nothing publicly, so test targets set their own `target_include_directories` for `src/`.
- `docs/` holds appliance documentation (e.g. `docs/pi-setup.md`, the verified Pi 5 + CV105 hardware profile). The README stays the how-to; docs/ is the record — don't duplicate commands between them.
- `cmake/CompilerWarnings.cmake` is an **empty placeholder**; warning flags (`-Wall -Wextra -Wpedantic`) are set directly on targets in the root `CMakeLists.txt`. Don't grep the module file for warning config.
- `cmake/Sanitizers.cmake` holds the optional sanitizers: `SUBTITLER_ENABLE_ASAN` / `SUBTITLER_ENABLE_UBSAN` (both default OFF), applied directory-scope so tests are instrumented too. Build them via the `asan-ubsan` configure/build/test presets (binary dir `build/asan-ubsan`).
- `cmake/Dependencies.cmake` declares GLib, libsoup, and Threads that nothing links yet — intentional (libsoup is for the planned web UI). Don't prune them as "unused".
- Wrap GStreamer objects with the RAII deleters in `src/stream/deleters.h` (see the `GstPointer` aliases in `main.cpp`) instead of raw `gst_*_unref` calls. Non-owning references (GObject casts, transfer-none getters) use `GstView<T>` — never wrap those in an owning `GstPointer`; the double-unref this causes was #134.
- Installation covers only the `subtitler` binary (`install(TARGETS ...)` + GNUInstallDirs in the root CMakeLists). When `config/subtidlerd.service` and `config/subtitler.example.json` get implemented, add install rules for them too.

## Gotchas

- Resolution/format constants (1080p60 YUY2) are **duplicated** in `src/main.cpp` and `src/stream/description.cpp` — change both together or capture/output caps mismatch.
- The output pipeline hardcodes `kmssink driver-name=vc4` (Raspberry Pi). Running the binary needs a real V4L2 capture device and a KMS display — don't use it as a smoke test on a dev machine; build-only verification is the norm.
- No vc4 plane supports packed 4:2:2 (no YUYV/UYVY/YVYU/VYUY — verified via `modetest -p`), so captured YUY2 frames **cannot be scanned out directly**: a conversion step (e.g. YUY2→NV16) before kmssink is mandatory. The empirical gst-launch confirmation pair is still pending; hardware profile in `docs/pi-setup.md`.
- Usage: `subtitler [video-device] [connector-id]` (defaults: `/dev/video0`, auto connector).

## OpenCode config

`opencode.json` pre-approves `cmake`/`ninja`/`ctest`, read-only git
(`status`/`diff`/`log`/`show`), `ls`, `rg`, and `gh issue view`; every other
bash command prompts. That's expected — don't work around the prompts.
