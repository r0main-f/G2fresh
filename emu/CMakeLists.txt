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
