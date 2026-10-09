add_library(g2core STATIC
  src/bitstream.cpp
  src/crc.cpp
  src/param_text.cpp
)
target_include_directories(g2core PUBLIC include)
