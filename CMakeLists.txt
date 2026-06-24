cmake_minimum_required(VERSION 3.14)
project(skibidi VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SKIBIDI_WITH_SQLITE
    "Build the optional SQLite compatibility backend and SQL benchmarks"
    OFF)

set(SKIBIDI_CORE_SOURCES
    src/cache.cpp
    src/codegen.cpp
    src/compiler.cpp
    src/lexer.cpp
    src/metadata.cpp
    src/native_engine.cpp
    src/native_index.cpp
    src/native_storage.cpp
    src/optimizer.cpp
    src/parser.cpp
)

if(SKIBIDI_WITH_SQLITE)
    find_package(SQLite3 QUIET)
    if(NOT SQLite3_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    endif()
    list(APPEND SKIBIDI_CORE_SOURCES src/executor.cpp)
endif()

add_library(skibidi_core ${SKIBIDI_CORE_SOURCES})
target_include_directories(skibidi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(SKIBIDI_WITH_SQLITE)
    target_include_directories(skibidi_core PUBLIC ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(skibidi_core PUBLIC ${SQLite3_LIBRARIES})
    target_compile_definitions(skibidi_core PUBLIC SKIBIDI_WITH_SQLITE=1)
endif()

function(skibidi_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

skibidi_warnings(skibidi_core)

add_executable(skibidi src/main.cpp)
target_link_libraries(skibidi PRIVATE skibidi_core)
skibidi_warnings(skibidi)

enable_testing()
add_subdirectory(test)
add_subdirectory(benchmarks)
