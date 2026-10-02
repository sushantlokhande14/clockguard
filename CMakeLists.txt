cmake_minimum_required(VERSION 3.16)
project(clockguard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(nlohmann_json 3 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

add_library(cg_core STATIC src/netlist.cpp src/analysis.cpp)
target_include_directories(cg_core PUBLIC src)
target_link_libraries(cg_core PUBLIC nlohmann_json::nlohmann_json)

add_executable(cg_engine src/main.cpp)
target_link_libraries(cg_engine PRIVATE cg_core)

enable_testing()
add_test(NAME designs COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/check_designs.py
         --engine $<TARGET_FILE:cg_engine> ${CMAKE_SOURCE_DIR}/designs)
