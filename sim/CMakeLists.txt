add_library(sim STATIC
  core/hello.cpp
  math/fixed.cpp
  vfs/path.cpp
  vfs/dir_depot.cpp
  vfs/memory_depot.cpp
  vfs/rwd_archive.cpp
  vfs/vfs.cpp
  data/tgi/diagnostics.cpp
  data/tgi/text_decode.cpp
  data/tgi/lexer.cpp
  data/tgi/symbols.cpp
  data/tgi/parser.cpp
  data/tgi/lst_loader.cpp
  data/tgi/template_table.cpp
  data/tgi/resolver.cpp
  data/tgi/records.cpp
  data/tgi/value_parse.cpp
  data/tgi/pipeline.cpp
  data/tgi/resolved_dump.cpp
)
target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sim PUBLIC cxx_std_20)
set_target_properties(sim PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(sim PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
