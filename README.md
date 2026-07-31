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
