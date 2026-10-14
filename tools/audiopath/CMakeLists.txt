# g2audiopath: measurements of the way between the emulated G2 and the host (re/notes/audio-path.md): the plugin's
# sample rate converter on a file and its cost, and (with the emulator) the Runner's buffer behind a real-time
# callback. aliasing.py and sweep.py use it.
add_executable(g2audiopath g2audiopath.cpp)
target_include_directories(g2audiopath PRIVATE ${PROJECT_SOURCE_DIR}/plugin/Source)  # Resampler.h (plain C++)
target_compile_features(g2audiopath PRIVATE cxx_std_20)
if(TARGET g2emu)
  target_link_libraries(g2audiopath PRIVATE g2emu)  # g2proto and g2core come with it
  target_compile_definitions(g2audiopath PRIVATE G2AUDIOPATH_EMU=1)
endif()
