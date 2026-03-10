cmake_minimum_required(VERSION 3.14)
project(skibidi VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find SQLite3
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    set(SQLite3_INCLUDE_DIRS ${SQLITE3_INCLUDE_DIRS})
    set(SQLite3_LIBRARIES ${SQLITE3_LIBRARIES})
endif()

# Gather sources
file(GLOB SOURCES "src/*.cpp")

add_executable(skibidi ${SOURCES})

target_include_directories(skibidi PRIVATE
    src/
    vendor/
    ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(skibidi PRIVATE ${SQLite3_LIBRARIES})

target_compile_options(skibidi PRIVATE -Wall -Wextra -Wpedantic)
