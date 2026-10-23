cmake_minimum_required(VERSION 3.22)
project(ProjectCorps VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One CRT everywhere (sim is linked into the GDExtension DLL, which godot-cpp also builds).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# RelWithDebInfo builds the optimized GDExtension (see below): full inlining, as in Release.
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

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
  # Six mixed regiments cross the test map (needs the game install; skipped without it), then the
  # recorded film must replay identically.
  add_test(NAME simcli_scenario COMMAND simcli scenario --out ${CMAKE_BINARY_DIR}/scenario.rpl)
  set_tests_properties(simcli_scenario PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP scenario_rpl)
  add_test(NAME simcli_scenario_determinism
    COMMAND simcli determinism-check ${CMAKE_BINARY_DIR}/scenario.rpl --per-stage)
  set_tests_properties(simcli_scenario_determinism PROPERTIES FIXTURES_REQUIRED scenario_rpl)
  # Combat (M5): a tank duel with a non-default tuning, then its film must replay identically.
  add_test(NAME simcli_duel COMMAND simcli duel regiment_ger_tank_medium regiment_gbr_infantry_at forest
    --tune defenseK=12 --out ${CMAKE_BINARY_DIR}/duel.rpl)
  set_tests_properties(simcli_duel PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP duel_rpl)
  add_test(NAME simcli_duel_determinism COMMAND simcli determinism-check ${CMAKE_BINARY_DIR}/duel.rpl --per-stage)
  set_tests_properties(simcli_duel_determinism PROPERTIES FIXTURES_REQUIRED duel_rpl)
  # The many-regiments benchmark runs (a short version; `simcli perf` alone is the 160-regiment one).
  add_test(NAME simcli_perf_smoke COMMAND simcli perf --gen 7 --size 512 --regiments 40 --ticks 60)
  set_tests_properties(simcli_perf_smoke PROPERTIES SKIP_RETURN_CODE 77)
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
  # godot-cpp 4.3 picks its CRT from CMAKE_BUILD_TYPE, which multi-config generators (Visual Studio)
  # leave empty, so it hardcodes /MDd in every config and the Release GDExtension fails to link
  # (_CrtDbgReport). Drop that flag; CMAKE_MSVC_RUNTIME_LIBRARY above picks the CRT per config.
  if(MSVC)
    get_target_property(_godot_cpp_flags godot-cpp COMPILE_FLAGS)
    if(_godot_cpp_flags)
      string(REPLACE "/MDd" "" _godot_cpp_flags "${_godot_cpp_flags}")
      set_target_properties(godot-cpp PROPERTIES COMPILE_FLAGS "${_godot_cpp_flags}")
    endif()
  endif()
  # godot-cpp 4.3 defines DEBUG_ENABLED / DEBUG_METHODS_ENABLED only in its Debug config, so its
  # Release config is a template_release build (no call argument checks), meant for export
  # templates. RelWithDebInfo is the optimized template_debug build (scons target=template_debug):
  # gdext/ writes it, like Debug, to godot/bin/<os>/debug/, the library the editor loads.
  target_compile_definitions(godot-cpp PUBLIC
    $<$<CONFIG:RelWithDebInfo>:DEBUG_ENABLED DEBUG_METHODS_ENABLED>)
  add_subdirectory(gdext)
endif()
