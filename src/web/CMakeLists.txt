add_library(
    web
    json_helpers.cpp
    opensubtitles_routes.cpp
    preview_routes.cpp
    static_files.cpp
    subtitle_routes.cpp
    web_server.cpp
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
)

target_include_directories(
    web
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
