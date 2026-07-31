find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(
    GLIB
    REQUIRED
    IMPORTED_TARGET
    glib-2.0
    gobject-2.0
    gio-2.0
)

pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gstreamer-video-1.0
)

pkg_check_modules(
    LIBSOUP
    REQUIRED
    IMPORTED_TARGET
    libsoup-3.0
)
