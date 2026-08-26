add_library(
    stream
    fonts.cpp
    frame_buffer.cpp
    preview_gate.cpp
    stream.cpp
    whisper_transcriber.cpp
)

add_library(
    subtitler::stream
    ALIAS stream
)

target_link_libraries(
    stream
    PRIVATE
    PkgConfig::ALSA
    PkgConfig::GSTREAMER
    PkgConfig::PANGO
    subtitler::utils
    subtitler_warnings
)

target_include_directories(
    stream
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

if(SUBTITLER_ENABLE_WHISPER)
    # PUBLIC: the test binaries compile against the same definition.
    target_compile_definitions(stream PUBLIC SUBTITLER_ENABLE_WHISPER)
    target_link_libraries(stream PRIVATE whisper)
endif()
