cmake_minimum_required(VERSION 3.20)
project(dynamic_hierarchy_deltas VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DELTA_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

add_library(delta_model src/model.cpp src/scalable.cpp)
target_include_directories(delta_model PUBLIC include)

if(DELTA_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(delta_model PRIVATE /W4 /permissive-)
  else()
    target_compile_options(delta_model PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endif()

add_executable(delta_demo apps/demo.cpp)
target_link_libraries(delta_demo PRIVATE delta_model)

add_executable(delta_exhaustive apps/exhaustive.cpp)
target_link_libraries(delta_exhaustive PRIVATE delta_model)

add_executable(delta_benchmark apps/benchmark.cpp)
target_link_libraries(delta_benchmark PRIVATE delta_model)

add_executable(delta_cli apps/delta_cli.cpp)
target_link_libraries(delta_cli PRIVATE delta_model)

add_executable(delta_scale_experiment apps/scale_experiment.cpp)
target_link_libraries(delta_scale_experiment PRIVATE delta_model)

enable_testing()
add_executable(delta_tests tests/test_model.cpp)
target_link_libraries(delta_tests PRIVATE delta_model)
add_test(NAME delta_tests COMMAND delta_tests)

add_executable(delta_scalable_tests tests/test_scalable.cpp)
target_link_libraries(delta_scalable_tests PRIVATE delta_model)
add_test(NAME delta_scalable_tests COMMAND delta_scalable_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME cpp_python_crosscheck
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/crosscheck_cpp.py
    --binary $<TARGET_FILE:delta_cli>
    --cases 150
    --nodes 6
    --seed 17
)

if(DELTA_ENABLE_WARNINGS)
  foreach(target delta_demo delta_exhaustive delta_benchmark delta_cli
                 delta_scale_experiment delta_tests delta_scalable_tests)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
  endforeach()
endif()
