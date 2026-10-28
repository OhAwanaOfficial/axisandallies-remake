add_executable(ai_tests
  main.cpp
  test_view.cpp
)
target_link_libraries(ai_tests PRIVATE corps_ai doctest::doctest)
target_compile_definitions(ai_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
if(MSVC)
  target_compile_options(ai_tests PRIVATE /W4 /permissive- /utf-8 /bigobj)
endif()
add_test(NAME ai_tests COMMAND ai_tests)
