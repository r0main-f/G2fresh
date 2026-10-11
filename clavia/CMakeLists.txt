# Reading the user's own copy of Clavia's free download (the .dmg, the resource files on it, the original editor's
# graphics): the emulator takes the G2 OS from it, the Classic look the editor's graphics. Nothing from Clavia is in
# the repository or the build.
add_library(g2clavia STATIC src/download.cpp src/skin.cpp)
target_include_directories(g2clavia PUBLIC include)
target_link_libraries(g2clavia PUBLIC g2core)
target_compile_features(g2clavia PUBLIC cxx_std_20)
set_target_properties(g2clavia PROPERTIES POSITION_INDEPENDENT_CODE ON)

# zlib reads the compressed .dmg Clavia ships the download in. macOS and Linux have it; elsewhere (Windows), or with
# -DG2_FETCH_ZLIB=ON, its inflate code is built from the zlib 1.3.1 release (zlib licence).
option(G2_FETCH_ZLIB "Build zlib's inflate from source instead of using the system's" OFF)
if(NOT G2_FETCH_ZLIB)
  find_package(ZLIB)
endif()
if(ZLIB_FOUND)
  target_link_libraries(g2clavia PRIVATE ZLIB::ZLIB)
else()
  include(FetchContent)
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
  target_link_libraries(g2clavia PRIVATE g2inflate)
endif()
