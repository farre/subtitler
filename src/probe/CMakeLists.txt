add_executable(
    subtitler-probe
    audio.cpp
    capture.cpp
    drm.cpp
    main.cpp
    pipeline.cpp
    plugins.cpp
    report.cpp
)

target_link_libraries(
    subtitler-probe
    PRIVATE
    PkgConfig::ALSA
    PkgConfig::GSTREAMER
    PkgConfig::LIBDRM
    subtitler::utils
)

target_include_directories(
    subtitler-probe
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_options(
    subtitler-probe
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

set_target_properties(
    subtitler-probe
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
