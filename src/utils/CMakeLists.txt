add_library(
    utils
    INTERFACE
    logging.h
    paths.h
    preview_frame.h
    reset_guard.h
    unique_ptr.h
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
