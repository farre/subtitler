add_library(subtitler_warnings INTERFACE)

target_compile_options(
    subtitler_warnings
    INTERFACE
        -Wall
        -Wextra
        -Wpedantic
)
