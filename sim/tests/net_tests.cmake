# M8 multiplayer tests. Included from sim/tests/CMakeLists.txt.
target_sources(sim_tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/test_lockstep.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test_packets.cpp
)
