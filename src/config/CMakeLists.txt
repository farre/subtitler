add_library(
    config
    config.cpp
)

add_library(
    subtitler::config
    ALIAS config
)

target_link_libraries(
    config
    PRIVATE
    PkgConfig::GLIB
    subtitler::utils
    subtitler_warnings
)

target_include_directories(
    config
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
