cmake_minimum_required(VERSION 3.20)
project(rdma_hotbucket_lstm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hotbucket_core
    src/cluster.cpp
    src/entity_memory.cpp
    src/policies.cpp
    src/replica_bundle.cpp
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

option(HOTBUCKET_ENABLE_RDMA "Build the Linux rdma_cm/libibverbs copy probe" OFF)
if(HOTBUCKET_ENABLE_RDMA)
    if(NOT UNIX)
        message(FATAL_ERROR "HOTBUCKET_ENABLE_RDMA requires Linux or another Unix RDMA stack")
    endif()
    find_path(RDMACM_INCLUDE_DIR rdma/rdma_cma.h REQUIRED)
    find_library(RDMACM_LIBRARY rdmacm REQUIRED)
    find_library(IBVERBS_LIBRARY ibverbs REQUIRED)
    add_executable(hotbucket_rdma_copy rdma/rdma_copy.cpp)
    target_include_directories(hotbucket_rdma_copy PRIVATE ${RDMACM_INCLUDE_DIR})
    target_link_libraries(hotbucket_rdma_copy PRIVATE ${RDMACM_LIBRARY} ${IBVERBS_LIBRARY})
    target_compile_options(
        hotbucket_rdma_copy PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )

    add_library(hotbucket_rdma_transport rdma/rdma_replication.cpp)
    target_include_directories(
        hotbucket_rdma_transport PRIVATE ${RDMACM_INCLUDE_DIR}
    )
    target_link_libraries(
        hotbucket_rdma_transport PUBLIC hotbucket_core ${RDMACM_LIBRARY} ${IBVERBS_LIBRARY}
    )
    target_compile_options(
        hotbucket_rdma_transport PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )

    add_executable(hotbucket_rdma_replica rdma/rdma_replica_main.cpp)
    target_link_libraries(hotbucket_rdma_replica PRIVATE hotbucket_rdma_transport)
    target_compile_options(
        hotbucket_rdma_replica PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )

    add_executable(hotbucket_rdma_sim src/main.cpp)
    target_compile_definitions(hotbucket_rdma_sim PRIVATE HOTBUCKET_HAS_RDMA=1)
    target_link_libraries(hotbucket_rdma_sim PRIVATE hotbucket_rdma_transport)
    target_compile_options(
        hotbucket_rdma_sim PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(hotbucket_tests tests/test_main.cpp)
    target_link_libraries(hotbucket_tests PRIVATE hotbucket_core)
    add_test(NAME hotbucket_tests COMMAND hotbucket_tests)
endif()
