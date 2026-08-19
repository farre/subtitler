find_package(doctest REQUIRED)

include(doctest)

add_executable(
    stream_tests
    description_tests.cpp
    frame_buffer_tests.cpp
    integration_tests.cpp
    output_anchor_tests.cpp
    output_pipeline_tests.cpp
)

target_link_libraries(
    stream_tests
    PRIVATE
    subtitler::stream
    doctest::doctest
    PkgConfig::GSTREAMER
)

target_compile_options(
    stream_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_include_directories(
    stream_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

doctest_discover_tests(stream_tests)

add_executable(
    web_tests
    preview_frame_buffer_tests.cpp
)

target_link_libraries(
    web_tests
    PRIVATE
    subtitler::utils
    doctest::doctest
    Threads::Threads
)

target_compile_options(
    web_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

doctest_discover_tests(web_tests)
