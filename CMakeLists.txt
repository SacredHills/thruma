cmake_minimum_required(VERSION 3.22)

# macOS: universal binary (Apple Silicon + Intel) by default; must be set before project()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    endif()
endif()

project(Thruma VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(THRUMA_BUILD_PLUGIN "Build the JUCE VST3 plugin" ON)
option(THRUMA_BUILD_TESTS  "Build the DSP test suite" ON)
set(THRUMA_JUCE_PATH "" CACHE PATH "Use a local JUCE checkout instead of downloading it")

# Windows: static runtime so the plugin has no MSVC redistributable dependency
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

#-------------------------------------------------------------------------------
# Core DSP library: pure C++17, no JUCE dependency, shared by plugin and tests.
add_subdirectory(Source/Core)

if(THRUMA_BUILD_PLUGIN)
    if(THRUMA_JUCE_PATH)
        add_subdirectory(${THRUMA_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        8.0.15
            GIT_SHALLOW    ON)
        FetchContent_MakeAvailable(JUCE)
    endif()
    add_subdirectory(Source/Plugin)
endif()

if(THRUMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

#-------------------------------------------------------------------------------
# Packaging (Linux .deb / .tar.gz). Windows (Inno Setup) and macOS (.pkg) installers
# are produced by the scripts in packaging/ - see docs/BUILDING.md.
if(THRUMA_BUILD_PLUGIN AND UNIX AND NOT APPLE)
    set(CPACK_PACKAGE_NAME "thruma")
    set(CPACK_PACKAGE_VENDOR "SacredHills")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Thruma - drum replacement and debleed VST3 plugin")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_CONTACT "SacredHills <https://github.com/SacredHills/thruma>")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libasound2 | libasound2t64, libfreetype6, libfontconfig1, libx11-6, libxext6")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_PACKAGE_FILE_NAME "Thruma-${PROJECT_VERSION}-Linux-x86_64")
    include(CPack)
endif()
