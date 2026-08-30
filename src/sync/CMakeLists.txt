add_library(
    sync
    srt_cues.cpp
    sync_matcher.cpp
    sync_session.cpp
)

add_library(
    subtitler::sync
    ALIAS sync
)

target_link_libraries(
    sync
    PRIVATE
    subtitler::utils
    subtitler_warnings
)

target_include_directories(
    sync
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
