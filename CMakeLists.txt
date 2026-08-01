cmake_minimum_required(VERSION 3.20)
project(rdma_hotbucket_lstm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hotbucket_core
    src/cluster.cpp
    src/policies.cpp
    src/workload.cpp
)
target_include_directories(hotbucket_core PUBLIC include)

if(MSVC)
    target_compile_options(hotbucket_core PRIVATE /W4)
else()
    target_compile_options(hotbucket_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(hotbucket_sim src/main.cpp)
target_link_libraries(hotbucket_sim PRIVATE hotbucket_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(hotbucket_tests tests/test_main.cpp)
    target_link_libraries(hotbucket_tests PRIVATE hotbucket_core)
    add_test(NAME hotbucket_tests COMMAND hotbucket_tests)
endif()
