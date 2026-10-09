cmake_minimum_required(VERSION 3.24)
project(G2fresh VERSION 0.3.0 LANGUAGES C CXX)

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
add_subdirectory(tools/g2tool)

if(G2_BUILD_PLUGIN)
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)

  # The bundled Clavia skin (see NOTICE), compiled in as binary data.
  file(GLOB G2_SKIN_FILES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/assets/clavia/panels.json
    ${PROJECT_SOURCE_DIR}/assets/clavia/cbmp/*.png
    ${PROJECT_SOURCE_DIR}/assets/clavia/jpeg/*.jpg)
  juce_add_binary_data(G2SkinData HEADER_NAME G2SkinData.h NAMESPACE G2SkinData SOURCES ${G2_SKIN_FILES})
  set_target_properties(G2SkinData PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # UI sources shared by the plugin/app and the render tool.
  set(G2_UI_SOURCES
    ${PROJECT_SOURCE_DIR}/ui/Skin.cpp
    ${PROJECT_SOURCE_DIR}/ui/ModulePainter.cpp
    ${PROJECT_SOURCE_DIR}/ui/PatchDocument.cpp
    ${PROJECT_SOURCE_DIR}/ui/AreaView.cpp
    ${PROJECT_SOURCE_DIR}/ui/ModuleBrowser.cpp
    ${PROJECT_SOURCE_DIR}/ui/MainView.cpp
    ${PROJECT_SOURCE_DIR}/ui/PatchSettingsBar.cpp
    ${PROJECT_SOURCE_DIR}/ui/Dialogs.cpp)

  add_subdirectory(plugin)
  add_subdirectory(tools/render)
endif()

if(G2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
