# AGENTS.md

## What this is

Headless HDMI capture appliance (C++26 + GStreamer): grabs 1080p60 video from a
V4L2 device and outputs it via DRM/KMS. Planned but NOT yet implemented: SRT
subtitle overlay, web controls, config file loading, systemd unit
(`config/subtitler.example.json` and `config/subtidlerd.service` are both
empty placeholders). Current code is the low-latency passthrough only.

## GitHub milestones

The roadmap lives in six GitHub milestones named `N. Title` (issue #1 was
split into issues #2–#29 across them). The numeric prefix defines execution
order — GitHub has no milestone ordering — so keep prefixes sequential when
adding or inserting milestones.

## Technology
C++26 is allowed! We use CMake and ninja.

## Build

```sh
cmake --preset default
cmake --build --preset default
```

- The `default` preset in `CMakePresets.json` sets the Ninja generator, `build/` binary dir, and clang++ — the README, the VSCode task, and manual builds all go through it.
- If `build/` already exists configured with Makefiles, delete it first — CMake errors on generator mismatch instead of switching in place.
- Requires CMake >= 3.30 and a very recent C++26 compiler (uses `std::print`, `std::format`, `std::out_ptr`, `std::jthread`). Verified working with Clang 22.1.8.
- Dependencies resolve via pkg-config (all `REQUIRED`; configure fails if dev packages are missing): gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0, gstreamer-video-1.0, glib-2.0 / gobject-2.0 / gio-2.0, libsoup-3.0. Tests additionally use doctest via `find_package(doctest REQUIRED)` (CMake package, not pkg-config).
- Tests run with `ctest --test-dir build` (doctest unit tests under `tests/`, registered via `doctest_discover_tests`; gated on `BUILD_TESTING`, default ON). No CI, no lint targets. Verification = a clean `cmake --build build` plus passing ctest.
- Formatting: clang-format with the repo's `.clang-format` (Google base, 2-space indent, 80 columns).

## Layout and conventions

- All headers are private and live next to their sources under `src/`. The include root is `src/`, so includes look like `#include "stream/description.h"`. The root `include/` directory is intentionally empty, reserved for a future public API — do not put headers there.
- `src/stream/` builds the static lib `stream` (alias `subtitler::stream`), consumed only by the `subtitler` executable from `src/main.cpp`. New modules should follow the same `src/<module>/` pattern with their own CMakeLists.
- `src/stream/stream.cpp` and `stream.h` are empty placeholders still listed as lib sources — the lib today is only `description.cpp` + `deleters.h`.
- `tests/` holds the doctest unit tests. Libs expose nothing publicly, so test targets set their own `target_include_directories` for `src/`.
- `cmake/CompilerWarnings.cmake` and `cmake/Sanitizers.cmake` are **empty placeholders**; warning flags (`-Wall -Wextra -Wpedantic`) are set directly on targets in the root `CMakeLists.txt`. Don't grep the module files for warning config.
- `cmake/Dependencies.cmake` declares GLib, libsoup, and Threads that nothing links yet — intentional (libsoup is for the planned web UI). Don't prune them as "unused".
- Wrap GStreamer objects with the RAII deleters in `src/stream/deleters.h` (see the `GstPointer` aliases in `main.cpp`) instead of raw `gst_*_unref` calls.

## Gotchas

- Resolution/format constants (1080p60 YUY2) are **duplicated** in `src/main.cpp` and `src/stream/description.cpp` — change both together or capture/output caps mismatch.
- The output pipeline hardcodes `kmssink driver-name=vc4` (Raspberry Pi). Running the binary needs a real V4L2 capture device and a KMS display — don't use it as a smoke test on a dev machine; build-only verification is the norm.
- Usage: `subtitler [video-device] [connector-id]` (defaults: `/dev/video0`, auto connector).

## OpenCode config

`opencode.json` pre-approves `cmake`/`ninja`/`ctest`, read-only git
(`status`/`diff`/`log`/`show`), `ls`, `rg`, and `gh issue view`; every other
bash command prompts. That's expected — don't work around the prompts.
