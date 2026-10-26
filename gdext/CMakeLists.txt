add_library(simbridge SHARED
  register_types.cpp
  sim_bridge.cpp
)
target_link_libraries(simbridge PRIVATE sim corps_ai godot-cpp)
# godot-cpp compiles with HOT_RELOAD_ENABLED as a private flag; class_db.hpp reads it too.
if(GODOT_ENABLE_HOT_RELOAD)
  target_compile_definitions(simbridge PRIVATE HOT_RELOAD_ENABLED)
endif()

# Output where godot/simbridge.gdextension expects it, by Godot template kind:
#   godot/bin/<windows|linux>/debug/    template_debug: Debug (unoptimized) or RelWithDebInfo (optimized)
#   godot/bin/<windows|linux>/release/  template_release: Release, MinSizeRel (exports only)
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _corps_os)
set(_corps_out "${CMAKE_SOURCE_DIR}/godot/bin/${_corps_os}/$<IF:$<CONFIG:Release,MinSizeRel>,release,debug>")
set_target_properties(simbridge PROPERTIES
  OUTPUT_NAME simbridge
  RUNTIME_OUTPUT_DIRECTORY "${_corps_out}"
  LIBRARY_OUTPUT_DIRECTORY "${_corps_out}"
  PREFIX "")
if(NOT WIN32)
  set_target_properties(simbridge PROPERTIES PREFIX "lib")
endif()
