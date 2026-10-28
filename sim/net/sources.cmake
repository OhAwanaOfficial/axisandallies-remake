# Multiplayer (M8, NETCODE.md): lockstep scheduling and the packet codec. Transport-free; the ENet
# transport lives on the Godot side. Included from sim/CMakeLists.txt.
target_sources(sim PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/lockstep.cpp
  ${CMAKE_CURRENT_LIST_DIR}/packets.cpp
)
