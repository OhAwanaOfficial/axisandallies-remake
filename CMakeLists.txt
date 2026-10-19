cmake_minimum_required(VERSION 3.22)
project(ProjectCorps VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One CRT everywhere (sim is linked into the GDExtension DLL, which godot-cpp also builds).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(CORPS_BUILD_GDEXT "Build the godot-cpp GDExtension (SimBridge)" ON)
option(CORPS_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

add_subdirectory(sim)
add_subdirectory(simcli)

if(CORPS_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
  # doctest 2.4.11 declares cmake_minimum_required(VERSION 3.0), which CMake >= 4.0 rejects.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_MakeAvailable(doctest)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  add_subdirectory(sim/tests)
  # The CLI path of the determinism check, on the committed synthetic replay (no game data needed).
  add_test(NAME simcli_determinism_check
    COMMAND simcli determinism-check ${CMAKE_SOURCE_DIR}/sim/tests/fixtures/synthetic_300.rpl --per-stage)
  # Same with nav traffic (blockers, path requests) on the generated map the replay names.
  add_test(NAME simcli_determinism_check_nav
    COMMAND simcli determinism-check ${CMAKE_SOURCE_DIR}/sim/tests/fixtures/synthetic_nav_300.rpl --per-stage)
  # HPA* quality bounds on a generated map (exits non-zero past mean 1.05x / worst 1.25x of optimal).
  add_test(NAME simcli_nav_bench
    COMMAND simcli nav-bench --gen 4 --size 192 --queries 150 --optimal)

  # Determinism guard: no float/double in sim/.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME no_float_in_sim
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_float.py ${CMAKE_SOURCE_DIR}/sim)
    add_test(NAME no_float_check_selftest
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_float.py --self-test)
    add_test(NAME trig_tables_current
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/gen_trig_tables.py --check)
    add_test(NAME tools_python_tests
      COMMAND ${Python3_EXECUTABLE} -I -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools/tests)
  else()
    message(WARNING "Python3 not found: no_float_in_sim check NOT registered. Install Python 3.11+.")
  endif()
endif()

if(CORPS_BUILD_GDEXT)
  FetchContent_Declare(godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG godot-4.3-stable
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/patch_godot_cpp.cmake)
  FetchContent_MakeAvailable(godot-cpp)
  add_subdirectory(gdext)
endif()
