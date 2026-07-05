cmake_minimum_required(VERSION 3.21)
project(itch_book CXX)

add_library(itch_book INTERFACE)
target_include_directories(itch_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itch_book INTERFACE cxx_std_23)

include(FetchContent)
FetchContent_Declare(fixed_decimal
  GIT_REPOSITORY https://github.com/groovg/fixed-decimal
  GIT_TAG ca1765785647e9118b6c9a4f6e173fbab53217fe
  SOURCE_SUBDIR cpp)
FetchContent_MakeAvailable(fixed_decimal)
target_link_libraries(itch_book INTERFACE fixed_decimal)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(parse_test test/parse_test.cpp)
  target_link_libraries(parse_test PRIVATE itch_book)
  target_compile_options(parse_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME parse COMMAND parse_test)

  add_executable(book_test test/book_test.cpp)
  target_link_libraries(book_test PRIVATE itch_book)
  target_compile_options(book_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME book COMMAND book_test)

  add_executable(differential_test test/differential_test.cpp)
  target_link_libraries(differential_test PRIVATE itch_book)
  target_compile_options(differential_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME differential COMMAND differential_test)

  add_executable(gen-synthetic tools/gen_synthetic.cpp)
  target_link_libraries(gen-synthetic PRIVATE itch_book)
  target_include_directories(gen-synthetic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_compile_options(gen-synthetic PRIVATE -Wall -Wextra -Werror)

  add_executable(itch-replay src/replay.cpp)
  target_link_libraries(itch-replay PRIVATE itch_book)
  target_compile_options(itch-replay PRIVATE -Wall -Wextra -Werror)
endif()
