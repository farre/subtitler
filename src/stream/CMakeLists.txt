add_library(
    stream
    description.cpp
    frame_buffer.cpp
    stream.cpp
)

add_library(
    subtitler::stream
    ALIAS stream
)

target_link_libraries(
    stream
    PRIVATE
    PkgConfig::GSTREAMER
)

target_include_directories(
    stream
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
