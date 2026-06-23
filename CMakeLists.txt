cmake_minimum_required(VERSION 3.21)
project(itch_book CXX)

add_library(itch_book INTERFACE)
target_include_directories(itch_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itch_book INTERFACE cxx_std_23)
