# Compiles XenonRecomp's output (ppc/, made from the player's own default.xex and never
# committed) into a static library. Regenerate it with:
#   build/tools/XenonRecomp/XenonRecomp/XenonRecomp config/bo2_sp.toml tools/XenonRecomp/XenonUtils/ppc_context.h
# Adapted from SpeedBreaker's recomp/CMakeLists.txt via the Deadpool port. GPL-3.0-or-later (see COPYING).
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB PPC_SOURCES ${ROOT}/ppc/*.cpp)
if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No generated sources in ppc/. Run XenonRecomp first.")
endif()

add_library(armb02RecompLib STATIC ${PPC_SOURCES})
target_compile_features(armb02RecompLib PUBLIC cxx_std_20)
target_include_directories(armb02RecompLib PUBLIC
    ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils
    ${ROOT}/tools/XenonRecomp/thirdparty/simde)
target_compile_options(armb02RecompLib PRIVATE
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
    -fno-strict-aliasing
    # Checked indirect calls and the game clock for mftb (runtime/cpu).
    "SHELL:-include \"${ROOT}/runtime/cpu/indirect.h\""
    "SHELL:-include \"${ROOT}/runtime/cpu/guest_time.h\""
    # MMIO stores: XMA command registers reach the XMA emulation.
    "SHELL:-include \"${ROOT}/runtime/cpu/mmio.h\"")
