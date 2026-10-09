add_executable(g2audio main.cpp)
target_link_libraries(g2audio PRIVATE g2engine)

add_executable(g2mkpatch g2mkpatch.cpp)
target_link_libraries(g2mkpatch PRIVATE g2core)
