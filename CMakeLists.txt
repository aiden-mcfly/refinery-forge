cmake_minimum_required(VERSION 3.16)
project(refinery-forge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PostgreSQL)

add_executable(refinery-forge
  src/forge/bitmask_generator.cpp
  src/forge/ledger.cpp
  src/forge/substrate_emit.cpp
  src/forge/tank.cpp
  third_party/xxhash/xxhash.c
)
set_source_files_properties(third_party/xxhash/xxhash.c PROPERTIES LANGUAGE C)
target_include_directories(refinery-forge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash
)

if(PostgreSQL_FOUND)
  target_compile_definitions(refinery-forge PRIVATE REFINERY_HAVE_LIBPQ=1)
  target_include_directories(refinery-forge PRIVATE ${PostgreSQL_INCLUDE_DIRS})
  target_link_libraries(refinery-forge PRIVATE PostgreSQL::PostgreSQL)
endif()
