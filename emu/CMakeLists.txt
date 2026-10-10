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

# A second fix, for GCC: on x86-64 outside Windows the library lists no callee-saved XMM registers as
# `JitReg128 g_nonVolatileXMMs[] = {}`, a zero-size array GCC rejects (clang accepts it). An empty std::array
# works with every use (range-for in jit.cpp's static_asserts and in jitstackhelper.cpp). Idempotent.
set(_jit_regs ${dsp56300_SOURCE_DIR}/source/dsp56kEmu/jitregtypes.h)
file(READ ${_jit_regs} _jit_regs_src)
if(NOT _jit_regs_src MATCHES "G2FRESH: empty std::array")
  string(REPLACE "static constexpr JitReg128 g_nonVolatileXMMs[] = {};"
                 "static constexpr std::array<JitReg128, 0> g_nonVolatileXMMs{};  // G2FRESH: empty std::array (GCC)"
                 _jit_regs_patched "${_jit_regs_src}")
  string(REPLACE "#include \"jittypes.h\"" "#include \"jittypes.h\"\n\n#include <array>" _jit_regs_patched "${_jit_regs_patched}")
  if(_jit_regs_patched STREQUAL _jit_regs_src)
    message(FATAL_ERROR "dsp56300: could not apply the zero-size array fix to ${_jit_regs}")
  endif()
  file(WRITE ${_jit_regs} "${_jit_regs_patched}")
endif()

if(NOT TARGET dsp56kEmu)
  # Only the libraries: the emulator, its base and asmjit (not its tests or tools).
  set(ASMJIT_STATIC TRUE)
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/asmjit ${dsp56300_BINARY_DIR}/asmjit EXCLUDE_FROM_ALL)
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/dsp56kBase ${dsp56300_BINARY_DIR}/dsp56kBase EXCLUDE_FROM_ALL)
  # dsp56kEmu links its JIT profiling helper (Intel's ITT API) on Windows and Linux; the library's top-level
  # CMakeLists adds it, which we skip.
  if(WIN32 OR (UNIX AND NOT APPLE))
    add_subdirectory(${dsp56300_SOURCE_DIR}/source/vtuneSdk ${dsp56300_BINARY_DIR}/vtuneSdk EXCLUDE_FROM_ALL)
  endif()
  add_subdirectory(${dsp56300_SOURCE_DIR}/source/dsp56kEmu ${dsp56300_BINARY_DIR}/dsp56kEmu EXCLUDE_FROM_ALL)
  # g2emu goes into the plugin, a shared library
  foreach(_t asmjit dsp56kBase dsp56kEmu vtuneSdk)
    if(TARGET ${_t})
      set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
  endforeach()
endif()

# g2dsppoc: runs one DSP fragment of the G2 OS on the emulated DSP56300.
add_executable(g2dsppoc dsppoc/g2dsppoc.cpp)
target_include_directories(g2dsppoc PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2dsppoc PRIVATE dsp56kEmu)
target_compile_features(g2dsppoc PRIVATE cxx_std_17)
target_compile_definitions(g2dsppoc PRIVATE _USE_MATH_DEFINES)  # M_PI with MSVC

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
target_compile_definitions(g2dspframe PRIVATE _USE_MATH_DEFINES)  # M_PI with MSVC

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

# A speed-up (G2FRESH): a window of plain RAM (the G2's SDRAM, where the OS runs) that the core fetches from, reads
# and writes directly instead of through the virtual Bus calls; everything else still goes to the bus. Edits the
# copies above; configure fails if an anchor is missing (a new pin).
function(g2_patch_file _file)
  file(READ ${_file} _src)
  if(_src MATCHES "G2FRESH")
    return()
  endif()
  math(EXPR _last "${ARGC} - 1")
  foreach(_i RANGE 1 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    set(_from "${ARGV${_i}}")
    set(_to "${ARGV${_j}}")
    string(FIND "${_src}" "${_from}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "g2_patch_file: anchor not found in ${_file}: ${_from}")
    endif()
    string(REPLACE "${_from}" "${_to}" _src "${_src}")
  endforeach()
  file(WRITE ${_file} "${_src}")
endfunction()
g2_patch_file(${_cf_dir}/cfCpu.h
  "		void setUnimplementedCallback(UnimplementedCallback _cb)"
  "		// G2FRESH: plain RAM at [_base, _base + _size) that is accessed directly, not through the bus
		void setFastMemory(uint8_t* _mem, const uint32_t _base, const uint32_t _size)
		{
			m_fastMem = _mem; m_fastBase = _base;
			m_fastEnd2 = _size >= 2 ? _size - 1 : 0; m_fastEnd4 = _size >= 4 ? _size - 3 : 0;
		}

		void setUnimplementedCallback(UnimplementedCallback _cb)"
  "		Bus& m_bus;"
  "		Bus& m_bus;
		uint8_t* m_fastMem = nullptr;	// G2FRESH: see setFastMemory
		uint32_t m_fastBase = 0, m_fastEnd2 = 0, m_fastEnd4 = 0;
		uint16_t fast16(const uint32_t _o) const { return static_cast<uint16_t>(m_fastMem[_o] << 8 | m_fastMem[_o + 1]); }
		uint32_t fast32(const uint32_t _o) const { return static_cast<uint32_t>(m_fastMem[_o]) << 24 | static_cast<uint32_t>(m_fastMem[_o + 1]) << 16 | static_cast<uint32_t>(m_fastMem[_o + 2]) << 8 | m_fastMem[_o + 3]; }")
g2_patch_file(${_cf_dir}/cfCpu.cpp
  "		const uint16_t w = m_bus.fetch16(m_pc);"
  "		const uint32_t fo = m_pc - m_fastBase;
		const uint16_t w = fo < m_fastEnd2 ? fast16(fo) : m_bus.fetch16(m_pc);"
  "			return m_bus.read8(_addr);"
  "			if(_addr - m_fastBase < m_fastEnd2) return m_fastMem[_addr - m_fastBase];
			return m_bus.read8(_addr);"
  "			return m_bus.read16(_addr);"
  "			if(_addr - m_fastBase < m_fastEnd2) return fast16(_addr - m_fastBase);
			return m_bus.read16(_addr);"
  "			return m_bus.read32(_addr);"
  "			if(_addr - m_fastBase < m_fastEnd4) return fast32(_addr - m_fastBase);
			return m_bus.read32(_addr);"
  "			m_bus.write8(_addr, static_cast<uint8_t>(_value));"
  "			if(_addr - m_fastBase < m_fastEnd2) { m_fastMem[_addr - m_fastBase] = static_cast<uint8_t>(_value); break; }
			m_bus.write8(_addr, static_cast<uint8_t>(_value));"
  "			m_bus.write16(_addr, static_cast<uint16_t>(_value));"
  "			if(_addr - m_fastBase < m_fastEnd2) { m_fastMem[_addr - m_fastBase] = static_cast<uint8_t>(_value >> 8); m_fastMem[_addr - m_fastBase + 1] = static_cast<uint8_t>(_value); break; }
			m_bus.write16(_addr, static_cast<uint16_t>(_value));"
  "			m_bus.write32(_addr, _value);"
  "			if(_addr - m_fastBase < m_fastEnd4) { uint8_t* p = m_fastMem + (_addr - m_fastBase); p[0] = static_cast<uint8_t>(_value >> 24); p[1] = static_cast<uint8_t>(_value >> 16); p[2] = static_cast<uint8_t>(_value >> 8); p[3] = static_cast<uint8_t>(_value); break; }
			m_bus.write32(_addr, _value);")
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
  g2emu/src/machine.cpp
  g2emu/src/panel.cpp
  g2emu/src/runner.cpp
  g2emu/src/dmg.cpp)
target_include_directories(g2emu PUBLIC g2emu/include)
target_include_directories(g2emu SYSTEM PRIVATE ${dsp56300_SOURCE_DIR}/source)
target_link_libraries(g2emu PUBLIC g2proto PRIVATE g2coldfire dsp56kEmu)
target_compile_features(g2emu PUBLIC cxx_std_20)
set_target_properties(g2emu PROPERTIES POSITION_INDEPENDENT_CODE ON)
# zlib reads the compressed .dmg Clavia ships the updater in. macOS and Linux have it; elsewhere (Windows), or with
# -DG2_FETCH_ZLIB=ON, its inflate code is built from the zlib 1.3.1 release (zlib licence).
option(G2_FETCH_ZLIB "Build zlib's inflate from source instead of using the system's" OFF)
if(NOT G2_FETCH_ZLIB)
  find_package(ZLIB)
endif()
if(ZLIB_FOUND)
  target_link_libraries(g2emu PRIVATE ZLIB::ZLIB)
else()
  FetchContent_Declare(g2zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
  FetchContent_GetProperties(g2zlib)
  if(NOT g2zlib_POPULATED)
    FetchContent_Populate(g2zlib)
  endif()
  set(_z ${g2zlib_SOURCE_DIR})
  add_library(g2inflate STATIC ${_z}/adler32.c ${_z}/crc32.c ${_z}/inffast.c ${_z}/inflate.c ${_z}/inftrees.c
                               ${_z}/uncompr.c ${_z}/zutil.c)
  target_include_directories(g2inflate SYSTEM PUBLIC ${_z})
  set_target_properties(g2inflate PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_definitions(g2inflate PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
  else()
    target_compile_options(g2inflate PRIVATE -w)
  endif()
  target_link_libraries(g2emu PRIVATE g2inflate)
endif()
target_compile_definitions(g2emu PRIVATE G2EMU_HAVE_ZLIB=1)

# g2emurun: boots the user's firmware, connects our protocol client, uploads patches, plays notes, records the DACs
# and reports the speed (the firmware-gated check of the whole machine).
add_executable(g2emurun g2emu/tools/g2emurun.cpp)
target_link_libraries(g2emurun PRIVATE g2emu g2core)
set(G2_DSP56300_SOURCE_DIR ${dsp56300_SOURCE_DIR} PARENT_SCOPE)
