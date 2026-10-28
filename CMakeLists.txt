cmake_minimum_required(VERSION 3.20)

project(armb02 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "XenonRecomp and its output are only tested with Clang 18 or later.")
endif()

# Host tools: XenonAnalyse, XenonRecomp, XenonUtils.
add_subdirectory(tools/XenonRecomp)

# Our own helper that pre-fills the recompiler config from the game executable.
add_subdirectory(tools/bo2scan)
