cmake_minimum_required(VERSION 3.10)
project(rewrite_link VERSION 0.1.0 LANGUAGES C CXX)

# rewrite_link.h is a single header. Use it from another CMake project with
#   add_subdirectory(link-c)   and   target_link_libraries(your_plugin PRIVATE rewrite_link)
# and put  #define REWRITE_LINK_IMPLEMENTATION  before the #include in exactly one source file.
add_library(rewrite_link INTERFACE)
target_include_directories(rewrite_link INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_libraries(rewrite_link INTERFACE ws2_32 bcrypt)
elseif(NOT APPLE)
  target_link_libraries(rewrite_link INTERFACE m)
endif()

option(REWRITE_LINK_BUILD_TESTS "Build the demo and the tests" ON)
if(NOT REWRITE_LINK_BUILD_TESTS)
  return()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(demo examples/demo.c examples/impl.c)
target_link_libraries(demo PRIVATE rewrite_link)

add_executable(host_example examples/host_example.c examples/game_stub.c)
target_link_libraries(host_example PRIVATE rewrite_link)
add_executable(guest_example examples/guest_example.c examples/game_stub.c)
target_link_libraries(guest_example PRIVATE rewrite_link)

add_executable(test_link tests/test_link.c)
target_link_libraries(test_link PRIVATE rewrite_link Threads::Threads)

# the header must also compile as C++ (the plugins this is for are mostly C++)
add_executable(cxx_check tests/cxx_check.cpp tests/cxx_other.cpp)
target_link_libraries(cxx_check PRIVATE rewrite_link)

enable_testing()
add_test(NAME unit COMMAND test_link)
add_test(NAME cxx COMMAND cxx_check)
