# subtitler
Headless HDMI video capture that overlays synchronized SRT subtitles in real time,  with web-based controls and automatic synchronization.

## Building

Requirements:

- CMake >= 3.30 and Ninja
- A C++26 compiler (verified with Clang 22.1.8)
- pkg-config modules: gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0,
  gstreamer-video-1.0, glib-2.0, gobject-2.0, gio-2.0, libsoup-3.0

On a 64-bit trixie-based Raspberry Pi OS, everything above — plus the test
framework and runtime GStreamer plugins — installs with:

```sh
sudo apt install clang cmake doctest-dev gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-good libdrm-dev libdrm-tests libglib2.0-dev \
    libgstreamer-plugins-base1.0-dev libgstreamer1.0-dev libsoup-3.0-dev \
    ninja-build pkg-config
```

(Bookworm is too old: it ships cmake 3.25 and clang 14, while the project
needs cmake >= 3.30 and a C++26-capable standard library.)

```sh
cmake --preset default
cmake --build --preset default
```

The `default` preset (see `CMakePresets.json`) uses the Ninja generator,
`build/` as the binary directory, and clang++ as the compiler. If `build/`
already exists configured with Makefiles, delete it first — CMake errors on
generator mismatch instead of switching in place.

Optional AddressSanitizer and UndefinedBehaviorSanitizer builds are available
via the `asan-ubsan` presets (binary dir `build/asan-ubsan`):

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

The underlying cache options are `SUBTITLER_ENABLE_ASAN` and
`SUBTITLER_ENABLE_UBSAN` (both default OFF).

## Testing

```sh
ctest --test-dir build
```

Tests use [doctest](https://github.com/doctest/doctest) (CMake package;
`doctest` on Arch, `doctest-dev` on Debian/Raspberry Pi OS). They are built
by default; disable with `-DBUILD_TESTING=OFF`.

## Installing

```sh
cmake --install build
```

Installs the `subtitler` binary to `${CMAKE_INSTALL_PREFIX}/bin`
(`/usr/local` by default).

## Provisioning the Pi

Create the service user and grant it device access via groups:

```sh
sudo adduser --system --no-create-home --shell /usr/sbin/nologin subtitler
sudo usermod -aG video,render,audio subtitler
```

The `video` group grants access to `/dev/video*` and `/dev/dri/card*`,
`render` to `/dev/dri/renderD*`, and `audio` to `/dev/snd/*`. Verify access
as the service user (capture device and display connected):

```sh
sudo -u subtitler v4l2-ctl --device /dev/video0 --all
sudo -u subtitler modetest -M vc4
```

## Diagnostics

`subtitler-probe` inspects the appliance's capture devices, GStreamer
elements, and DRM/KMS capabilities using native APIs:

```sh
subtitler-probe [--json] [devices | capture <device> | plugins | drm]
```

With no arguments it prints a full report; `--json` emits machine-readable
output.

## Running

```sh
./build/subtitler [video-device] [connector-id]
```

- `video-device` — V4L2 capture device (default `/dev/video0`). It must
  deliver 1920x1080 YUY2 at 60 fps; the capture format is hardcoded.
- `connector-id` — DRM connector ID of the display output (optional; kmssink
  picks a connector automatically if omitted). List connector IDs with
  `modetest -M vc4`.

Video is output directly via KMS/DRM using the vc4 driver (Raspberry Pi), so
run from a virtual console where no X/Wayland compositor owns the display,
with permission to access `/dev/dri` (root or membership in the `video`
group). The GStreamer `v4l2src` and `kmssink` elements (gst-plugins-good and
gst-plugins-bad) must be installed.

Stop with Ctrl+C; the program prints how many frames its internal buffer
dropped while running.
