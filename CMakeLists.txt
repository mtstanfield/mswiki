cmake_minimum_required(VERSION 3.16)
project(mswiki LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSWIKI_STATIC "Build a statically linked binary" OFF)

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

enable_testing()
add_test(NAME mswiki_self_test COMMAND mswiki --self-test)
