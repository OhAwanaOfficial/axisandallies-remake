add_executable(sim_tests
  main.cpp
  test_hello.cpp
  test_vfs.cpp
  test_math.cpp
  test_sim.cpp
  test_tgi_parse.cpp
  test_tgi_values.cpp
  test_tgi_lst.cpp
  test_tgi_resolve.cpp
  test_tgi_golden.cpp
)
target_link_libraries(sim_tests PRIVATE sim doctest::doctest)
# Golden tests read <source>/config/game_path.cfg and skip when it is missing.
target_compile_definitions(sim_tests PRIVATE CORPS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  DOCTEST_CONFIG_USE_STD_HEADERS)  # string_view comparisons need the real <ostream>
if(MSVC)
  target_compile_options(sim_tests PRIVATE /W4 /permissive- /utf-8 /bigobj)
endif()
add_test(NAME sim_tests COMMAND sim_tests)
