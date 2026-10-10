include(FetchContent)
# Built from source so universal (arm64 + x86_64) and cross-platform builds work.
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(g2tests
  test_bitstream.cpp
  test_edit.cpp
  test_file.cpp
  test_patch.cpp
  test_replace.cpp
  test_special.cpp
  test_mutate.cpp
  test_patch_load.cpp
  test_uprate.cpp
  test_param_text.cpp
  test_param_text_golden.cpp
  test_graphs.cpp
  test_proto_framing.cpp
  test_proto_messages.cpp
  test_proto_session.cpp
  test_proto_link.cpp
  test_proto_log.cpp
  test_bridge.cpp
  test_engine.cpp
)
target_link_libraries(g2tests PRIVATE g2bridgelib g2engine Catch2::Catch2WithMain) # g2proto and g2core come with it
target_compile_definitions(g2tests PRIVATE G2_BRIDGE_EXECUTABLE="$<TARGET_FILE:g2bridge>")
add_dependencies(g2tests g2bridge)
if(TARGET g2usb)
  target_link_libraries(g2tests PRIVATE g2usb)
  target_compile_definitions(g2tests PRIVATE G2_HAVE_USB=1)
endif()
target_compile_definitions(g2tests PRIVATE
  G2_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
  G2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_definitions(g2tests PRIVATE G2_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

include(Catch)
catch_discover_tests(g2tests)

# Checks that need Python: the generated module database is in sync with
# data/*.json, and the reference decoder round-trips the corpus.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mutate_param_classes_are_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/mutate/gen_param_classes.py --check)
  add_test(NAME replace_tables_are_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/moduledb/gen_replace.py --check)
  add_test(NAME patch_load_table_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/patchload/gen_table.py --check)
  add_test(NAME protocol_vectors_are_current
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/protocol/gen_cpp.py --check)
  add_test(NAME module_db_generated_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/moduledb/gen_cpp.py --check)
  file(GLOB G2_CORPUS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/corpus/pch2csd/*.pch2)
  add_test(NAME reference_decoder_roundtrip
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pch2/pch2dump.py roundtrip ${G2_CORPUS_FILES})
endif()

# The real-time G2 emulator (emu/g2emu, built with -DG2_BUILD_EMU=ON). Its [firmware] case boots the user's own OS
# when it finds it (G2_FIRMWARE, or original/firmware) and is skipped otherwise.
if(TARGET g2emu)
  add_executable(g2emutests test_g2emu.cpp)
  target_link_libraries(g2emutests PRIVATE g2emu g2coldfire dsp56kEmu Catch2::Catch2WithMain)
  target_include_directories(g2emutests PRIVATE ${PROJECT_SOURCE_DIR}/emu/g2emu/src)
  target_include_directories(g2emutests SYSTEM PRIVATE ${G2_DSP56300_SOURCE_DIR}/source)
  target_compile_definitions(g2emutests PRIVATE G2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  catch_discover_tests(g2emutests)
endif()
