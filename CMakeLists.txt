cmake_minimum_required(VERSION 3.24)
project(G2fresh VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(G2_BUILD_PLUGIN "Build the JUCE editor (stand-alone, VST3, AU)" ON)
option(G2_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(core)

if(G2_BUILD_PLUGIN)
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
  add_subdirectory(plugin)
endif()

if(G2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
