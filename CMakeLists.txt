cmake_minimum_required(VERSION 3.16)
project(mswiki LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSWIKI_STATIC "Build a statically linked binary" OFF)
option(MSWIKI_FUZZING "Build libFuzzer targets" OFF)

add_executable(mswiki src/main.cpp)

target_compile_options(mswiki PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
  -fno-exceptions
  -fno-rtti
)

target_link_libraries(mswiki PRIVATE sqlite3)

if (MSWIKI_STATIC)
  target_link_options(mswiki PRIVATE -static)
endif()

if (MSWIKI_FUZZING)
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MSWIKI_FUZZING requires clang/clang++")
  endif()

  add_executable(mswiki_http_fuzz src/fuzz_http_request.cpp)
  target_compile_options(mswiki_http_fuzz PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
    -O1
    -g
    -fno-omit-frame-pointer
    -fsanitize=fuzzer,address,undefined
  )
  target_link_options(mswiki_http_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(mswiki_http_fuzz PRIVATE sqlite3)
endif()

enable_testing()
add_test(NAME mswiki_self_test COMMAND mswiki --self-test)
