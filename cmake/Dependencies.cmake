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
    LIBDRM
    REQUIRED
    IMPORTED_TARGET
    libdrm
)

pkg_check_modules(
    ALSA
    REQUIRED
    IMPORTED_TARGET
    alsa
)

pkg_check_modules(
    LIBSOUP
    REQUIRED
    IMPORTED_TARGET
    libsoup-3.0
)

# Font enumeration for the subtitle renderer (#159).
pkg_check_modules(
    PANGO
    REQUIRED
    IMPORTED_TARGET
    pangocairo
)

# The whisper speech-recognition tap (#19 spike): whisper.cpp, fetched and
# built from source. ON by default — whisper is part of the standard build
# and --whisper=<model> is the runtime opt-in; OFF only for offline or
# minimal builds (the tap module then compiles to a stub and --whisper
# just fails model loading). The first configure of a build dir clones
# whisper.cpp, so it needs network.
option(SUBTITLER_ENABLE_WHISPER "Fetch whisper.cpp and enable the whisper audio tap" ON)

if(SUBTITLER_ENABLE_WHISPER)
    include(FetchContent)

    FetchContent_Declare(
        whisper
        GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
        GIT_TAG v1.9.3
        GIT_SHALLOW TRUE
    )

    # CACHE FORCE: whisper.cpp's own cmake_minimum_required resets CMP0077
    # to OLD in its scope, which would ignore plain variables.
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # whisper.cpp defaults BUILD_SHARED_LIBS to ON; keep the project's
    # libraries static so the binaries stay self-contained.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(whisper)
endif()
