# g2engine: a native audio engine for G2 patches (module processors written
# from the module catalog, re/notes/dsp-module-catalog.md).
add_library(g2engine STATIC
  src/engine.cpp
  modules/all.cpp
  modules/keyboard.cpp
  modules/out2.cpp
  modules/out4.cpp
  modules/osc_a.cpp
  modules/flt_lp.cpp
  modules/env_adsr.cpp
  modules/mix4_1a.cpp
  modules/mix2_1a.cpp
)
target_include_directories(g2engine PRIVATE modules)
target_include_directories(g2engine PUBLIC include)
target_link_libraries(g2engine PUBLIC g2core)
set_target_properties(g2engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
