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
