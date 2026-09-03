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

include(CompilerCache)
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)
include(GNUInstallDirs)

include(CTest)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

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
    config
    stream
    web
    subtitler::utils
)

target_include_directories(
    subtitler
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

install(
    TARGETS subtitler subtitler-probe
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# Static web assets (#212), resolved at runtime as <data-dir>/subtitler/web.
install(
    DIRECTORY web
    DESTINATION "${CMAKE_INSTALL_DATADIR}/subtitler"
)

# The documented example configuration (#16); copy to
# <config-dir>/subtitler/config.ini.
install(
    FILES config/subtitler.example.ini
    DESTINATION "${CMAKE_INSTALL_DATADIR}/subtitler"
)

# The systemd unit (#431). Not GNUInstallDirs' CMAKE_INSTALL_LIBDIR: on
# Debian that carries the multiarch triplet, and units don't belong
# under it. <prefix>/lib/systemd/system is in the unit search path for
# both /usr and /usr/local.
install(
    FILES config/subtitler.service
    DESTINATION "lib/systemd/system"
)

# The Debian package. dpkg-buildpackage drives its own debhelper build
# (debian/rules configures obj-<triplet> and runs the tests), so this
# is a thin wrapper; the .deb lands next to the source dir.
find_program(DPKG_BUILDPACKAGE_EXECUTABLE dpkg-buildpackage)
if(DPKG_BUILDPACKAGE_EXECUTABLE)
    add_custom_target(
        deb
        COMMAND
            "${DPKG_BUILDPACKAGE_EXECUTABLE}" -us -uc -b
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Building the Debian package (artifacts land next to the source directory)"
        USES_TERMINAL
        VERBATIM
    )
else()
    message(STATUS "dpkg-buildpackage not found; the 'deb' target is unavailable")
endif()
