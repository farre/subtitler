add_library(
    stream
    frame_buffer.cpp
    output_anchor.cpp
    preview_gate.cpp
    stream.cpp
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
    subtitler::utils
)

target_include_directories(
    stream
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
