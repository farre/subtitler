add_executable(
    subtitler-test
    main.cpp
)

# The workbench exercises exactly the production sync module; nothing
# else, so matcher changes made here carry over to the appliance.
target_link_libraries(
    subtitler-test
    PRIVATE
    subtitler::sync
)

target_include_directories(
    subtitler-test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_options(
    subtitler-test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

set_target_properties(
    subtitler-test
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
