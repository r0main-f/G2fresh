add_library(g2core STATIC
  src/bitstream.cpp
  src/file.cpp
  src/module_db.cpp
  src/module_db_data.cpp
  src/patch.cpp
  src/crc.cpp
  src/edit.cpp
  src/param_text.cpp
)
target_include_directories(g2core PUBLIC include)
