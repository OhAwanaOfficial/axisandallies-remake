# Host-side player controllers (docs/AI.md): read the sim, emit Commands. Not part of sim/.
add_library(corps_ai STATIC
  controller.cpp
)
target_link_libraries(corps_ai PUBLIC sim)
target_include_directories(corps_ai PUBLIC ${CMAKE_SOURCE_DIR})
if(MSVC)
  target_compile_options(corps_ai PRIVATE /W4 /permissive-)
else()
  target_compile_options(corps_ai PRIVATE -Wall -Wextra -Wpedantic)
endif()
