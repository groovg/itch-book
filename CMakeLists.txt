cmake_minimum_required(VERSION 3.21)
project(itch_book CXX)

add_library(itch_book INTERFACE)
target_include_directories(itch_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itch_book INTERFACE cxx_std_23)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(parse_test test/parse_test.cpp)
  target_link_libraries(parse_test PRIVATE itch_book)
  target_compile_options(parse_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME parse COMMAND parse_test)
endif()
