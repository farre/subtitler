find_package(doctest REQUIRED)

include(doctest)

add_executable(
    stream_tests
    description_tests.cpp
)

target_link_libraries(
    stream_tests
    PRIVATE
    subtitler::stream
    doctest::doctest
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
