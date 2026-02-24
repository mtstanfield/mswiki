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

  function(add_mswiki_fuzz_target target_name source_file)
    add_executable(${target_name} ${source_file})
    target_compile_options(${target_name} PRIVATE
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
    target_link_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address,undefined
    )
    target_link_libraries(${target_name} PRIVATE sqlite3)
  endfunction()

  add_mswiki_fuzz_target(mswiki_http_fuzz src/fuzz_http_request.cpp)
  add_mswiki_fuzz_target(mswiki_markdown_fuzz src/fuzz_markdown.cpp)
  add_mswiki_fuzz_target(mswiki_multipart_fuzz src/fuzz_multipart.cpp)
endif()

enable_testing()
add_test(NAME mswiki_self_test COMMAND $<TARGET_FILE:mswiki> --self-test)
