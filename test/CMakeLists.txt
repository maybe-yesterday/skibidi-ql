cmake_minimum_required(VERSION 3.14)

# Source files shared by all test executables (exclude main.cpp)
file(GLOB LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# Include paths: src/ headers, vendor/ (nlohmann/json), test/ (test_framework.h)
set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SQLite3_INCLUDE_DIRS}
)

# Helper macro to register one test executable
macro(add_skibidi_test name)
    add_executable(${name} ${name}.cpp ${LIB_SOURCES})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${name} PRIVATE ${SQLite3_LIBRARIES})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

add_skibidi_test(test_lexer)
add_skibidi_test(test_parser)
add_skibidi_test(test_codegen)
add_skibidi_test(test_optimizer)
add_skibidi_test(test_metadata)
