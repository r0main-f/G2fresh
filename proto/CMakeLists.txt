# libg2proto: the G2's USB protocol (host side) and a virtual G2 for tests, with
# no USB library dependency. Spec: re/notes/usb-protocol.md.
add_library(g2proto STATIC
  src/frame.cpp
  src/molecules.cpp
  src/bubble.cpp
  src/sections.cpp
  src/state.cpp
  src/client.cpp
  src/emulator.cpp
  src/link.cpp
  src/logging_transport.cpp
)
target_include_directories(g2proto PUBLIC include)
target_link_libraries(g2proto PUBLIC g2core)
# Linked into plugin shared objects too.
set_target_properties(g2proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
