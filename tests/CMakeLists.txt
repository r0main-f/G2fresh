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
  test_param_text.cpp
  test_param_text_golden.cpp
)
target_link_libraries(g2tests PRIVATE g2core Catch2::Catch2WithMain)
target_compile_definitions(g2tests PRIVATE G2_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_definitions(g2tests PRIVATE G2_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

include(Catch)
catch_discover_tests(g2tests)
