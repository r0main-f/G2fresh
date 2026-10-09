# g2bridge: the process that owns the G2's USB connection and serves every
# editor (stand-alone app, plugin instances) over a local socket.
#   g2bridgelib  the server, the editor side (BridgeLink) and the wire format;
#                no USB dependency, linked by the editors
#   g2bridge     the executable (USB through g2usb, or --emulator)
add_library(g2bridgelib STATIC
  src/socket.cpp
  src/wire.cpp
  src/server.cpp
  src/bridge_link.cpp
  src/spawn.cpp
)
target_include_directories(g2bridgelib PUBLIC include)
target_link_libraries(g2bridgelib PUBLIC g2proto)
if(WIN32)
  target_link_libraries(g2bridgelib PUBLIC ws2_32)
endif()
set_target_properties(g2bridgelib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(g2bridge main.cpp)
target_link_libraries(g2bridge PRIVATE g2bridgelib)
if(TARGET g2usb)
  target_link_libraries(g2bridge PRIVATE g2usb)
  target_compile_definitions(g2bridge PRIVATE G2_HAVE_USB=1)
endif()
