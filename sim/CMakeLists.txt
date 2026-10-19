add_library(sim STATIC
  core/hello.cpp
  math/fixed.cpp
  math/angle.cpp
  math/fvec2.cpp
  ecs/entity.cpp
  ecs/world.cpp
  core/command.cpp
  core/sim.cpp
  systems/stages.cpp
  session/hash.cpp
  session/state_io.cpp
  session/replay.cpp
  data/defs.cpp
  data/def_compiler.cpp
  data/json_lite.cpp
  nav/map_def.cpp
  nav/nav_grid.cpp
  nav/grid_search.cpp
  nav/hpa.cpp
  nav/path_service.cpp
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
# xxHash (BSD-2) for the state hash; vendored outside sim/ so the no-float check doesn't scan it.
target_include_directories(sim PRIVATE ${CMAKE_SOURCE_DIR}/third_party/xxhash)

# Sim version for replays/lobbies (NETCODE §7): git tree hash of sim/ at configure time, "-dirty"
# when sim/ has uncommitted changes. Re-run CMake configure to refresh it.
find_package(Git QUIET)
set(CORPS_SIM_VERSION "unknown")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD:sim
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE _sim_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _sim_rc ERROR_QUIET)
  if(_sim_rc EQUAL 0)
    set(CORPS_SIM_VERSION "${_sim_tree}")
    execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD -- sim
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE _sim_dirty ERROR_QUIET)
    if(NOT _sim_dirty EQUAL 0)
      set(CORPS_SIM_VERSION "${CORPS_SIM_VERSION}-dirty")
    endif()
  endif()
endif()
set_source_files_properties(session/replay.cpp PROPERTIES COMPILE_DEFINITIONS "CORPS_SIM_VERSION=\"${CORPS_SIM_VERSION}\"")
target_compile_features(sim PUBLIC cxx_std_20)
set_target_properties(sim PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(sim PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
