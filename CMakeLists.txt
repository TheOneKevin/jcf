cmake_minimum_required(VERSION 3.21)

project(OLKCompiler VERSION 1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_policy(SET CMP0135 NEW)
include(FetchContent)
enable_testing()

# Option to always produce colored output
option (FORCE_COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." TRUE)
if (${FORCE_COLORED_OUTPUT})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
       add_compile_options (-fdiagnostics-color=always)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
       add_compile_options (-fcolor-diagnostics)
    endif()
endif()

# Set debug mode flags
string(CONCAT CMAKE_CXX_FLAGS_DEBUG
    " ${CMAKE_CXX_FLAGS_DEBUG}"
    # The warnings to enable and ignore
    " -Wall"
    " -Wextra"
    " -Wpedantic"
    " -Wno-unused-function"
    " -Wno-unused-private-field"
    # Emit debug symbols
    " -g"
    " -gdwarf-4"
    " -O0"
    # Macro to enable debug mode
    " -DDEBUG"
    " -DENABLE_STACKTRACE"
    # Add the sanitizers too
    " -fsanitize=undefined"
    " -fsanitize=address"
    " -fno-omit-frame-pointer"
)

# Set the linker flags for debug mode
string(CONCAT CMAKE_EXE_LINKER_FLAGS_DEBUG
    "${CMAKE_EXE_LINKER_FLAGS_DEBUG}"
)

# Set release mode flags
string(CONCAT CMAKE_CXX_FLAGS_RELEASE
    "${CMAKE_CXX_FLAGS_RELEASE}"
    # Turn on optimizations and emit debug symbols
    " -O3"
    " -g"
)

# Set the bison debug mode flags
set(BISON_COMPILE_FLAGS "-v --locations -k")
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    string(CONCAT BISON_COMPILE_FLAGS
        "${BISON_COMPILE_FLAGS}"
        " --report=state --html --graph --color=always"
    )
endif()

# Define YYDEBUG for bison
add_compile_definitions(YYDEBUG=1)

# On macOS, libc++ removed std::result_of in C++20. We still use it in our code.
if(APPLE)
    add_compile_definitions(_LIBCPP_ENABLE_CXX20_REMOVED_TYPE_TRAITS)
endif()

################################################################################
#                         Flex and Bison targets                               #
################################################################################

find_package(FLEX 2.6 REQUIRED)
find_package(BISON 3.8 REQUIRED)

# Needed to override yyFlexLexer
if(NOT FLEX_INCLUDE_DIR)
    get_filename_component(_flex_bin_dir "${FLEX_EXECUTABLE}" DIRECTORY)
    find_path(FLEX_INCLUDE_DIR FlexLexer.h HINTS "${_flex_bin_dir}/../include")
endif()
if(FLEX_INCLUDE_DIR)
    message(STATUS "Using FlexLexer.h from: ${FLEX_INCLUDE_DIR}")
    include_directories(BEFORE "${FLEX_INCLUDE_DIR}")
endif()

set(PARSER_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(LEXER_OUT "${PARSER_DIR}/joos1w_lexer.cpp")
set(PARSER_OUT "${PARSER_DIR}/joos1w_parser.cpp")

flex_target(
    LEXER
    "lib/grammar/joos1w_lexer.l"
    "${LEXER_OUT}"
    DEFINES_FILE
    "${PARSER_DIR}/joos1w.lexer.do.not.use.h"
)

bison_target(
    PARSER
    "lib/grammar/joos1w_parser.y"
    "${PARSER_OUT}"
    DEFINES_FILE
    "${PARSER_DIR}/joos1w.parser.tab.h"
    COMPILE_FLAGS
    "${BISON_COMPILE_FLAGS}"
)

add_flex_bison_dependency(LEXER PARSER)

################################################################################
#                          Testing and macros                                  #
################################################################################

FetchContent_Declare(
    googletest
    URL
    https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Create a library target for our compiler with sources under lib/
file(GLOB_RECURSE LIB_SOURCES "lib/*.cc" "passes/*.cc")
add_library(OLKCompilerLib
    # flex bison
    "${LEXER_OUT}"
    "${PARSER_OUT}"
    # the rest of the library sources
    ${LIB_SOURCES}
)
target_include_directories(
    OLKCompilerLib
    PUBLIC
    "include"
    "${PARSER_DIR}"
)

# Macro to add an executable with lexer and parser sources
macro(add_tool TARGET_NAME)
    add_executable(${TARGET_NAME}
        ${ARGN}
    )
    target_link_libraries(${TARGET_NAME} OLKCompilerLib)
    target_include_directories(${TARGET_NAME} PRIVATE
        "${PARSER_DIR}"
        "include"
        "."
    )
endmacro()

# Macro to add a test executable with lexer and parser sources
macro(add_my_test TARGET_NAME)
    add_tool(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} GTest::gtest_main)
    gtest_discover_tests(${TARGET_NAME})
endmacro()

################################################################################
# Include backward
################################################################################

add_subdirectory("lib/third-party/backward-cpp")
target_link_libraries(OLKCompilerLib PUBLIC Backward::Interface)

################################################################################
#            Run python program right now to generate the tests                #
################################################################################

# execute_process(
#     COMMAND python3 "tests/generate_gtest.py"
#     WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
# )
# 
# file(GLOB GENERATED_TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tests/generated/*.cc)

################################################################################
#              For every tool, create a new add_*() call below                 #
################################################################################

add_tool(
    scanner
    "tools/scanner/main.cc"
)

add_tool(
    parser
    "tools/parser/main.cc"
)

add_tool(
    jcc1
    "tools/jcc1/main.cc"
    "tools/jcc1/msgprinter.cc"
)

add_tool(
    crashtest
    "tools/crashtest/main.cc"
)

add_tool(
    tirtest
    "tools/tirtest/main.cc"
)
