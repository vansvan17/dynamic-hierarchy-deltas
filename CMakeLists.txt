cmake_minimum_required(VERSION 3.20)
project(dynamic_hierarchy_deltas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dh main.cpp)
target_include_directories(dh PRIVATE .)
target_compile_options(dh PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME unit COMMAND dh test)
add_test(NAME exhaustive COMMAND dh exhaustive 5)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME python_crosscheck
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/oracle/crosscheck_cpp.py
                 --binary $<TARGET_FILE:dh> --cases 150 --nodes 6 --seed 17)
