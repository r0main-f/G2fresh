add_library(g2core STATIC
  src/bitstream.cpp
  src/file.cpp
  src/module_db.cpp
  src/module_db_data.cpp
  src/patch.cpp
  src/crc.cpp
  src/edit.cpp
  src/param_text.cpp
  src/graphs.cpp
)
target_include_directories(g2core PUBLIC include)
# Linked into plugin shared objects too.
set_target_properties(g2core PROPERTIES POSITION_INDEPENDENT_CODE ON)
