# The hardware bench pack (bench/hardware-pack): test patches for friends who own a G2, our predictions from the
# emulated G2, and the analysis of what they send back. See bench/hardware-pack/NOTES.md.

# g2benchpack: writes the pack's patches and manifest with the core's edit API (reproducible, no Clavia content).
add_executable(g2benchpack g2benchpack.cpp)
target_link_libraries(g2benchpack PRIVATE g2core)

# g2benchrender: plays one patch or performance on the emulated G2 (the user's own OS) and records its four outputs.
if(TARGET g2emu)
  add_executable(g2benchrender g2benchrender.cpp)
  target_link_libraries(g2benchrender PRIVATE g2emu)  # g2proto and g2core come with it
endif()
