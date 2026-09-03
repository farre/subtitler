# subtitler
Headless HDMI video capture that overlays synchronized SRT subtitles in real time,  with web-based controls and automatic synchronization.

## Building

Requirements:

- CMake >= 3.30 and Ninja
- A C++26 compiler (verified with Clang 22.1.8)
- pkg-config modules: gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0,
  gstreamer-video-1.0, glib-2.0, gobject-2.0, gio-2.0, libsoup-3.0, pangocairo

On a 64-bit trixie-based Raspberry Pi OS, everything above — plus the test
framework and runtime GStreamer plugins — installs with:

```sh
sudo apt install clang cmake doctest-dev gstreamer1.0-alsa \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good libasound2-dev libdrm-dev libdrm-tests \
    libglib2.0-dev libgstreamer-plugins-base1.0-dev libgstreamer1.0-dev \
    libpango1.0-dev libsoup-3.0-dev ninja-build pkg-config
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

### Debian package (the Pi)

```sh
sudo apt install dpkg-dev debhelper   # plus the packages from Building
cmake --preset default
cmake --build --preset default --target deb
sudo apt install ../subtitler_0.1.0_arm64.deb
```

The `deb` target wraps `dpkg-buildpackage -us -uc -b` (it exists only
when dpkg-buildpackage is installed); the .deb lands next to the source
directory. The package build runs the test suite and needs network
access: debhelper forbids downloads during configure, so `debian/rules`
clones whisper.cpp into the package build dir first. The package installs
the binaries, the web assets, and the example configuration; creates
the `subtitler` service user (systemd-sysusers, so the manual
provisioning below is already done); and installs, enables, and starts
`subtitler.service`. The service runs `subtitler --web` as the service
user with configuration in `/etc/subtitler/config.ini` and state in
`/var/lib/subtitler`, restarting on failure; manage it with
`systemctl status subtitler` and `journalctl -u subtitler`.

### Manual install

```sh
cmake --install build
```

Installs the binaries, the web assets, the example configuration, and
the systemd unit under `${CMAKE_INSTALL_PREFIX}` (`/usr/local` by
default).

## Provisioning the Pi

Skip this section when installing the deb — it creates the user. For a
manual install, create the service user and grant it device access via
groups:

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
subtitler-probe [--json] [devices | capture <device> | audio | plugins | drm | pipeline]
```

With no arguments it prints a full report; `--json` emits machine-readable
output.

## Running

With the deb installed the appliance is already running as
`subtitler.service`; this section is for running from the build tree.

```sh
./build/subtitler [video-device] [connector-id] [--config=<path>] [--output=software|pisp|window|null] [--no-audio] [--audio-output-device=<alsa-device>] [--audio-offset=<ms>] [--subtitles=<srt-file>] [--web] [--web-root=<dir>]
```

- `video-device` — V4L2 capture device (default `/dev/video0`). It must
  deliver 1920x1080 YUY2 at 60 fps; the capture format is hardcoded.
- `connector-id` — DRM connector ID of the display output (optional; kmssink
  picks a connector automatically if omitted). List connector IDs with
  `modetest -M vc4`.
- `--output` — output backend (default `software`):
  - `software` — converts to NV16 with `videoconvert` and scans out via
    `kmssink`; the lossless correctness reference.
  - `pisp` — converts to NV12 SAND-tiled DMABuf with the PiSP hardware
    converter (`pispconvert`, from `gstreamer1.0-pispconvert`) and scans out
    zero-copy via `kmssink`. Pi 5 only.
  - `window` — `glimagesink`, for dev machines with a display server.
  - `null` — `fakesink`, for headless testing.
- `--no-audio` — omit the audio branches entirely (CV105 ALSA capture on
  `hw:CARD=Video,DEV=0` and HDMI audio output), for machines without the
  CV105's audio device.
- `--audio-output-device` — ALSA playback device for the audio output branch
  (default `hw:CARD=vc4hdmi0,DEV=0`, the Pi's vc4-hdmi). Captured HDMI audio
  is forwarded bit-transparently (S16LE/48kHz/stereo, `slave-method=skew`
  drift correction).
- `--web` — start the web server on port 8080: static files from the web
  root (browse to `http://<host>:8080/`), the live 640x360 10 fps MJPEG
  preview (`/api/preview.mjpeg`) or a single frame (`/api/preview.jpg`),
  and the subtitle upload endpoint — `PUT /api/subtitles/<title>.srt`
  with the raw SRT as the body stores it in the state dir's library
  (sharded by the title's first letter), marks it active for the next
  boot, and switches it in live. Preview encoding runs only while at
  least one client is connected; without clients the preview endpoints
   serve a magenta placeholder frame. The full API is documented in the
   [REST API reference](docs/rest-api.md).
- `--web-root=<dir>` — directory the static files are served from
  (`.html`/`.js`/`.css`/`.png` only). Default: the first
  `<data-dir>/subtitler/web` found across `$XDG_DATA_HOME` and
  `$XDG_DATA_DIRS` (e.g. `/usr/local/share/subtitler/web` after
  installation). Use `--web-root=web` to serve the repo's `web/`
   directory in development.

## Configuration

Every command-line option can instead live in an INI file at
`$XDG_CONFIG_HOME/subtitler/config.ini` (usually
`~/.config/subtitler/config.ini`; `--config=<path>` overrides). The
command line wins over the file. The subtitle state changed through the
web API — selected file, visibility, delay, and cue font family, size,
and color — is written back atomically, so the appliance restores it
after a reboot. `subtitler.example.ini` (installed next to the web
assets under `<data-dir>/subtitler/`) documents every key.

Structured logging is opt-in via the `SUBTITLER_LOG` environment variable: a
comma-separated list of `<label>:<level>` entries, where level is `error`,
`warn`, `info`, or `debug` (enabling that level and above for the label).
Labels are named per directory: `config`, `main`, `stream`, `web`; the catch-all label
`all` enables every label (a label's own entry wins over `all`). Example:
`SUBTITLER_LOG="all:info,stream:debug" ./build/subtitler --web`. Fatal errors
are always printed regardless of `SUBTITLER_LOG`.

The KMS modes output directly via KMS/DRM using the vc4 driver (Raspberry
Pi), so run from a virtual console where no X/Wayland compositor owns the
display, with permission to access `/dev/dri` (root or membership in the
`video` group). The vc4 planes cannot scan out packed YUY2, so the output
pipeline converts each frame before `kmssink` (see docs/pi-setup.md). The
GStreamer `v4l2src` (gst-plugins-good), `videoconvert` (gst-plugins-base),
and `kmssink` (gst-plugins-bad) elements must be installed.

Stop with Ctrl+C; the program logs how many frames its internal buffer
dropped while running (`main:info`).
