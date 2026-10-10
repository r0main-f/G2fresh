# Experiments toward running the G2's own DSP code (see
# re/notes/g2-hardware-and-emulation.md). Off by default: built with
# -DG2_BUILD_EMU=ON. Uses Gearmulator's DSP56300 emulator (GPL-3.0), fetched at
# a pinned commit; nothing from Clavia is in the repository: the tools read the
# user's own firmware (tools/firmware/).
include(FetchContent)
FetchContent_Declare(dsp56300
  GIT_REPOSITORY https://github.com/dsp56300/dsp56300.git
  GIT_TAG 123804394c1669847962a808e39f69e83c2752a9
  GIT_SUBMODULES source/asmjit)
FetchContent_GetProperties(dsp56300)
if(NOT dsp56300_POPULATED)
  FetchContent_Populate(dsp56300)
endif()

# A fix to the library (re/notes/g2-hardware-and-emulation.md 3.8). In a build that has the JIT
# (g_useJIT), DSP::execInterrupt runs every interrupt vector through the JIT, also when the DSP
# runs on the interpreter, as ours do. The interpreter computes the condition codes E, U and N
# lazily (ccrCache); the JIT reads SR as it is, so a jsr in the vector stacked an SR without the
# pending N of the instruction before the interrupt, and the rti brought back the stale bits. A
# Bcc right after a cmp went the wrong way whenever an interrupt fell in between: the G2's
# background loop then ran its control-rate code every 2 to 3 frames instead of every 4.
# Resolving the pending bits before the JIT runs the vector fixes it (idempotent edit).
set(_dsp_cpp ${dsp56300_SOURCE_DIR}/source/dsp56kEmu/dsp.cpp)
file(READ ${_dsp_cpp} _dsp_src)
if(NOT _dsp_src MATCHES "G2FRESH: flush the lazy CCR")
  string(REPLACE "			LOGJITPC(vba);\n"
                 "			LOGJITPC(vba);\n			updateDirtyCCR();  // G2FRESH: flush the lazy CCR before the JIT stacks SR\n"
                 _dsp_patched "${_dsp_src}")
  if(_dsp_patched STREQUAL _dsp_src)
    message(FATAL_ERROR "dsp56300: could not apply the interrupt CCR fix to ${_dsp_cpp}")
  endif()
  file(WRITE ${_dsp_cpp} "${_dsp_patched}")
endif()

if(NOT TARGET dsp56kEmu)
  # Only the libraries: the emulator, its base and asmjit (not its tests or tools).
  set(ASMJIT_STATIC TRUE)
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/asmjit ${dsp56300_BINARY_DIR}/asmjit EXCLUDE_FROM_ALL)
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/dsp56kBase ${dsp56300_BINARY_DIR}/dsp56kBase EXCLUDE_FROM_ALL)
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/dsp56kEmu ${dsp56300_BINARY_DIR}/dsp56kEmu EXCLUDE_FROM_ALL)
endif()

# g2dsppoc: runs one DSP fragment of the G2 OS on the emulated DSP56300.
add_executable(g2dsppoc dsppoc/g2dsppoc.cpp)
target_include_directories(g2dsppoc PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2dsppoc PRIVATE dsp56kEmu)
target_compile_features(g2dsppoc PRIVATE cxx_std_17)

# g2dspbridge: a C interface to emulated DSP56367s for tools/firmware/g2hostemu.py
# (the ColdFire OS in Unicorn drives them through their HDI08 host ports).
add_library(g2dspbridge SHARED dspbridge/g2dspbridge.cpp)
target_include_directories(g2dspbridge PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2dspbridge PRIVATE dsp56kEmu)
target_compile_features(g2dspbridge PRIVATE cxx_std_17)

# g2protobridge: G2fresh's protocol client (proto/) behind a C interface, so that
# g2hostemu.py can connect it to the emulated G2's USB chip.
add_library(g2protobridge SHARED protobridge/g2protobridge.cpp)
target_link_libraries(g2protobridge PRIVATE g2proto)
target_compile_features(g2protobridge PRIVATE cxx_std_20)

# g2dspframe: runs one DSP's per-frame program offline from the memories
# g2hostemu.py dumps after a patch upload, one sample per frame.
add_executable(g2dspframe dspframe/g2dspframe.cpp)
target_include_directories(g2dspframe PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2dspframe PRIVATE dsp56kEmu)
target_compile_features(g2dspframe PRIVATE cxx_std_17)

# g2dspccr: checks the library fix above (an interrupt between cmp and Bcc keeps the branch);
# run build/emu/g2dspccr (exit code 0 = fixed).
add_executable(g2dspccr dsptest/g2dspccr.cpp)
target_include_directories(g2dspccr PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2dspccr PRIVATE dsp56kEmu)
target_compile_features(g2dspccr PRIVATE cxx_std_17)

# ---------------------------------------------------------------------------------------------------------------
# g2emu: the real-time C++ emulator of the whole G2 (re/notes/g2-hardware-and-emulation.md 3.9): the user's own OS
# on Gearmulator's ColdFire core, the board's hardware, four DSP56367s on dsp56300's JIT, USB and MIDI.
#
# Gearmulator's ColdFire core (source/cpu/coldfire/cfCpu.*, GPL-3.0, https://github.com/dsp56300/gearmulator) is
# fetched file by file at a pinned commit: the repository's archive is 160 MB and we need two files of it.
set(G2_GEARMULATOR_COMMIT fd3777fb63ecd6d4697706dfb3d4ada97953454f)
set(_gm_raw https://raw.githubusercontent.com/dsp56300/gearmulator/${G2_GEARMULATOR_COMMIT}/source/cpu/coldfire)
FetchContent_Declare(gm_cfcpu_cpp URL ${_gm_raw}/cfCpu.cpp DOWNLOAD_NO_EXTRACT TRUE
  URL_HASH SHA256=ea959630200344fe9e419868d8950a14c8edc0124c5ce5c4195c6dfc8128ba51)
FetchContent_Declare(gm_cfcpu_h URL ${_gm_raw}/cfCpu.h DOWNLOAD_NO_EXTRACT TRUE
  URL_HASH SHA256=5852758e419fc6174bf4074df1c078b0c023cd5ce0584286c59b9bfef8df2e89)
foreach(_gm gm_cfcpu_cpp gm_cfcpu_h)
  FetchContent_GetProperties(${_gm})
  if(NOT ${_gm}_POPULATED)
    FetchContent_Populate(${_gm})
  endif()
endforeach()
# laid out as Gearmulator has them (#include "coldfire/cfCpu.h")
set(_cf_dir ${CMAKE_CURRENT_BINARY_DIR}/gearmulator/coldfire)
configure_file(${gm_cfcpu_cpp_SOURCE_DIR}/cfCpu.cpp ${_cf_dir}/cfCpu.cpp COPYONLY)
configure_file(${gm_cfcpu_h_SOURCE_DIR}/cfCpu.h ${_cf_dir}/cfCpu.h COPYONLY)
add_library(g2coldfire STATIC ${_cf_dir}/cfCpu.cpp)
target_include_directories(g2coldfire SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/gearmulator)
target_compile_features(g2coldfire PUBLIC cxx_std_17)
set_target_properties(g2coldfire PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(g2coldfire PRIVATE -w)
endif()

add_library(g2emu STATIC
  g2emu/src/firmware.cpp
  g2emu/src/hw.cpp
  g2emu/src/dsp.cpp
  g2emu/src/machine.cpp)
target_include_directories(g2emu PUBLIC g2emu/include)
target_include_directories(g2emu SYSTEM PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2emu PUBLIC g2proto PRIVATE g2coldfire dsp56kEmu)
target_compile_features(g2emu PUBLIC cxx_std_20)
set_target_properties(g2emu PROPERTIES POSITION_INDEPENDENT_CODE ON)

# g2emurun: boots the user's firmware, connects our protocol client, uploads patches, plays notes, records the DACs
# and reports the speed (the firmware-gated check of the whole machine).
add_executable(g2emurun g2emu/tools/g2emurun.cpp)
target_link_libraries(g2emurun PRIVATE g2emu g2core)
