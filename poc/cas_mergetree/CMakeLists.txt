cmake_minimum_required(VERSION 3.16)
project(cas_mergetree_poc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(cas STATIC cas.cpp)
target_include_directories(cas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(cas_tests tests.cpp)
target_link_libraries(cas_tests PRIVATE cas)

enable_testing()
add_test(NAME cas_tests COMMAND cas_tests)
