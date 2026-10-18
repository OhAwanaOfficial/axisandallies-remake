# FetchContent PATCH_COMMAND for godot-cpp 4.3 (runs with the godot-cpp source dir as cwd).
#
# godot-cpp's "Generating bindings" rule passes every path inside one `python -c "...(\"<path>\")"`
# argument. On Windows the VS generator runs it through cmd.exe, whose quote parsing flips at each
# inner \", so an `&` in the checkout path (".../Axis & Allies/...") splits the command. Pass the
# arguments as separate argv entries instead; they need no inner quotes. Idempotent.

set(_file "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
file(READ "${_file}" _text)

set(_old [=["-c" "import binding_generator; binding_generator.generate_bindings(\"${GODOT_GDEXTENSION_API_FILE}\", \"${GENERATE_BINDING_PARAMETERS}\", \"${BITS}\", \"${FLOAT_PRECISION}\", \"${CMAKE_CURRENT_BINARY_DIR}\")"]=])
set(_new [=["-c" "import sys, binding_generator; binding_generator.generate_bindings(*sys.argv[1:])" "${GODOT_GDEXTENSION_API_FILE}" "${GENERATE_BINDING_PARAMETERS}" "${BITS}" "${FLOAT_PRECISION}" "${CMAKE_CURRENT_BINARY_DIR}"]=])

string(FIND "${_text}" "${_new}" _already)
if(_already GREATER -1)
  return()
endif()
string(FIND "${_text}" "${_old}" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR "patch_godot_cpp: binding_generator rule not found; update this patch for the new godot-cpp version")
endif()
string(REPLACE "${_old}" "${_new}" _text "${_text}")
file(WRITE "${_file}" "${_text}")
message(STATUS "patch_godot_cpp: binding generator now takes its paths as argv")
