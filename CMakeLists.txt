cmake_minimum_required(VERSION 3.14)
project(viper)
add_compile_options()
set(CMAKE_CXX_STANDARD 17)

### OPTIONS ###
option(VIPER_BUILD_PLAYGROUND "Set ON if playground should be built." OFF)

option(VIPER_BUILD_BENCHMARKS "Set OM if benchmarks should be built." OFF)

option(VIPER_BUILD_TESTS "Set ON to build HiOM integration tests under test/" OFF)

option(VIPER_CONCURRENT_QUEUE_PROVIDED "Set ON if the concurrentqueue dependency is provided and should not be
                                        downloaded by Viper." OFF)

set(VIPER_PMDK_PATH "/usr" CACHE STRING "Path to custom PMDK install directory")

###############

# VIPER
add_library(viper INTERFACE)
target_include_directories(viper INTERFACE include/)
target_compile_options(viper INTERFACE  -mclwb)

# CONCURRENTQUEUE
if (NOT ${VIPER_CONCURRENT_QUEUE_PROVIDED})
    include(FetchContent)
    FetchContent_Declare(
            concurrentqueue

            GIT_REPOSITORY https://gitee.com/mirrors/concurrentqueue.git
            GIT_TAG v1.0.3
    )
    FetchContent_MakeAvailable(concurrentqueue)
    target_link_libraries(viper INTERFACE concurrentqueue)
endif()

# VIPER PLAYGROUND
if (${VIPER_BUILD_PLAYGROUND})
    add_executable(playground playground.cpp)

    target_compile_options(playground PRIVATE  -mclwb -pthread)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
    if (CMAKE_BUILD_TYPE MATCHES Release)
        message(STATUS "BUILDING RELEASE")
        target_compile_options(playground PRIVATE -O3 -Ofast -mtune=native)
    endif()

    target_link_libraries(playground viper pthread)
endif()

# VIPER BENCHMARKS
if (${VIPER_BUILD_BENCHMARKS})
    add_subdirectory(benchmark)
endif()

# VIPER HIOM TESTS
if (${VIPER_BUILD_TESTS})
    add_executable(hiom_integration_test test/hiom_integration_test.cpp)
    target_compile_options(hiom_integration_test PRIVATE -mclwb -pthread)
    if (CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(hiom_integration_test PRIVATE -O3 -mtune=native)
    endif()
    target_link_libraries(hiom_integration_test viper pthread)
endif()
