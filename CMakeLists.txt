cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "The oldest macOS the game runs on")
endif()
project(armb02 VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "XenonRecomp and its output are only tested with Clang 18 or later.")
endif()

# XenonRecomp's bundled fmt uses malloc/free without <cstdlib>, which clang 23's
# headers no longer pull in for it.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cstdlib>)

# Host tools: XenonAnalyse, XenonRecomp, XenonUtils.
add_subdirectory(tools/XenonRecomp)

# Our own helper that pre-fills the recompiler config from the game executable.
add_subdirectory(tools/bo2scan)

# The game itself: the recompiled executable (recomp/, from ppc/) on the runtime
# adapted from the Deadpool port (runtime/). Off until you have generated ppc/.
option(ARMB02_GAME "Build the game (needs ppc/ from XenonRecomp)" OFF)
if(ARMB02_GAME)
    # XMA audio needs FFmpeg-XMA in thirdparty/ffmpeg-xma: on when it is built.
    if(EXISTS ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg-xma/lib/libavcodec.a)
        set(_armb02_xma_default ON)
    else()
        set(_armb02_xma_default OFF)
    endif()
    option(ARMB02_XMA "Decode XMA audio with FFmpeg-XMA (thirdparty/ffmpeg-xma); off = silence" ${_armb02_xma_default})
    add_subdirectory(recomp)
    add_subdirectory(runtime)
endif()
