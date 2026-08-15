add_library(
    utils
    INTERFACE
    reset_guard.h
)

add_library(
    subtitler::utils
    ALIAS utils
)

target_include_directories(
    utils
    INTERFACE
        "${PROJECT_SOURCE_DIR}/src"
)
