add_executable(simcli main.cpp sim_commands.cpp nav_bench.cpp scenario.cpp perf.cpp duel.cpp economy.cpp skirmish.cpp)
target_link_libraries(simcli PRIVATE sim)

if(MSVC)
  target_compile_options(simcli PRIVATE /W4 /permissive-)
else()
  target_compile_options(simcli PRIVATE -Wall -Wextra -Wpedantic)
endif()
