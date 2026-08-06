add_executable(
    subtitler-probe
    capture.cpp
    drm.cpp
    main.cpp
    plugins.cpp
    report.cpp
)

target_link_libraries(
    subtitler-probe
    PRIVATE
    PkgConfig::GSTREAMER
    PkgConfig::LIBDRM
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
