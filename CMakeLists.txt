cmake_minimum_required(VERSION 3.30)

project(
    subtitler
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src)

add_executable(
    subtitler
    src/main.cpp
)

target_compile_features(
    subtitler
    PRIVATE
    cxx_std_26
)

target_compile_options(
    subtitler
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_link_libraries(
    subtitler
    PRIVATE
    PkgConfig::GSTREAMER
    stream
)

target_include_directories(
    subtitler
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
