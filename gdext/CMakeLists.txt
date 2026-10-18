add_library(simbridge SHARED
  register_types.cpp
  sim_bridge.cpp
)
target_link_libraries(simbridge PRIVATE sim godot-cpp)

# Output where godot/simbridge.gdextension expects it:
#   godot/bin/<windows|linux>/<debug|release>/
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _corps_os)
set(_corps_out "${CMAKE_SOURCE_DIR}/godot/bin/${_corps_os}/$<IF:$<CONFIG:Debug>,debug,release>")
set_target_properties(simbridge PROPERTIES
  OUTPUT_NAME simbridge
  RUNTIME_OUTPUT_DIRECTORY "${_corps_out}"
  LIBRARY_OUTPUT_DIRECTORY "${_corps_out}"
  PREFIX "")
if(NOT WIN32)
  set_target_properties(simbridge PROPERTIES PREFIX "lib")
endif()
