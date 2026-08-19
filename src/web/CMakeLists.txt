add_library(
    web
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
