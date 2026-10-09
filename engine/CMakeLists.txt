# g2engine: a native audio engine for G2 patches (module processors written
# from the module catalog, re/notes/dsp-module-catalog.md).
add_library(g2engine STATIC
  src/engine.cpp
  modules/all.cpp
)
target_include_directories(g2engine PUBLIC include)
target_link_libraries(g2engine PUBLIC g2core)
set_target_properties(g2engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
