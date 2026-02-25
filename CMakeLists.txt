cmake_minimum_required(VERSION 3.16)
project(mswiki LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSWIKI_STATIC "Build a statically linked binary" OFF)
option(MSWIKI_FUZZING "Build libFuzzer targets" OFF)
option(MSWIKI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MSWIKI_STRICT_CLANG "Enable strict clang warning profile" ON)

add_executable(mswiki src/main.cpp)

set(MSWIKI_WARNING_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
)

if (MSWIKI_STRICT_CLANG AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND MSWIKI_WARNING_FLAGS
    -Weverything
    # Project is C++11-only; compatibility-to-C++98 warnings are not actionable.
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    # Struct padding warnings are noisy and not correctness issues for this app.
    -Wno-padded
    # Some developer environments inject host include paths that trigger this.
    -Wno-poison-system-directories
  )
  # Clang 16+ enables unsafe-buffer diagnostics under -Weverything.
  # This codebase intentionally uses bounded C-style buffers for C++11
  # portability, so this warning family is currently too noisy to gate on.
  if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    list(APPEND MSWIKI_WARNING_FLAGS -Wno-unsafe-buffer-usage)
  endif()
endif()

if (MSWIKI_WARNINGS_AS_ERRORS)
  list(APPEND MSWIKI_WARNING_FLAGS -Werror)
endif()

target_compile_options(mswiki PRIVATE
  ${MSWIKI_WARNING_FLAGS}
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
      ${MSWIKI_WARNING_FLAGS}
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
