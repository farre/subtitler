# subtitler
Headless HDMI video capture that overlays synchronized SRT subtitles in real time,  with web-based controls and automatic synchronization.

## Building

Requirements:

- CMake >= 3.30 and Ninja
- A C++26 compiler (verified with Clang 22.1.8)
- pkg-config modules: gstreamer-1.0, gstreamer-app-1.0, gstreamer-audio-1.0,
  gstreamer-video-1.0, glib-2.0, gobject-2.0, gio-2.0, libsoup-3.0

```sh
cmake --preset default
cmake --build --preset default
```

The `default` preset (see `CMakePresets.json`) uses the Ninja generator,
`build/` as the binary directory, and clang++ as the compiler. If `build/`
already exists configured with Makefiles, delete it first — CMake errors on
generator mismatch instead of switching in place.

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
