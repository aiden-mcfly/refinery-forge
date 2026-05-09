cmake_minimum_required(VERSION 3.16)
project(refinery-forge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(refinery-forge
  src/forge/bitmask_generator.cpp
  third_party/xxhash/xxhash.c
)
set_source_files_properties(third_party/xxhash/xxhash.c PROPERTIES LANGUAGE C)
target_include_directories(refinery-forge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash
)
