# Use buildcache as the compiler launcher when available; a launcher
# set from the command line or a preset always wins. The project itself
# is CXX-only, but the FetchContent'd whisper.cpp/ggml builds C too.
find_program(BUILDCACHE_EXECUTABLE buildcache)
if(BUILDCACHE_EXECUTABLE)
    if(NOT DEFINED CMAKE_C_COMPILER_LAUNCHER)
        set(CMAKE_C_COMPILER_LAUNCHER "${BUILDCACHE_EXECUTABLE}")
    endif()
    if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${BUILDCACHE_EXECUTABLE}")
    endif()
endif()
