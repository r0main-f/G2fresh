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
  test_uprate.cpp
  test_param_text.cpp
  test_param_text_golden.cpp
  test_graphs.cpp
)
target_link_libraries(g2tests PRIVATE g2core Catch2::Catch2WithMain)
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
  add_test(NAME module_db_generated_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/moduledb/gen_cpp.py --check)
  file(GLOB G2_CORPUS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/corpus/pch2csd/*.pch2)
  add_test(NAME reference_decoder_roundtrip
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pch2/pch2dump.py roundtrip ${G2_CORPUS_FILES})
endif()
