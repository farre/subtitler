add_library(
    web
    font_routes.cpp
    json_helpers.cpp
    opensubtitles_routes.cpp
    preview_routes.cpp
    static_files.cpp
    subtitle_routes.cpp
    transcript_routes.cpp
    web_server.cpp
    whisper_routes.cpp
)

add_library(
    subtitler::web
    ALIAS web
)

target_link_libraries(
    web
    PRIVATE
    PkgConfig::LIBSOUP
    subtitler::utils
    subtitler_warnings
)

target_include_directories(
    web
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
