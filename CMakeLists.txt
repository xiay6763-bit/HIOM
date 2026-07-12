cmake_minimum_required(VERSION 3.14)
project(viper)
add_compile_options()
set(CMAKE_CXX_STANDARD 17)

### OPTIONS ###
option(VIPER_BUILD_PLAYGROUND "Set ON if playground should be built." OFF)

option(VIPER_BUILD_BENCHMARKS "Set OM if benchmarks should be built." OFF)

option(VIPER_BUILD_TESTS "Set ON to build HiOM integration tests under test/" OFF)

option(VIPER_BUILD_DESTRUCTIVE_TESTS
       "Set ON to register destructive tests (real SIGKILL crash recovery:
        removes /pmem0/hiom/sigkill_test, creates a 1 GiB pool, SIGKILLs
        child processes) with ctest. The target still builds under
        VIPER_BUILD_TESTS; only the ctest registration is gated." OFF)

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
    # Enable CTest at top level BEFORE the benchmark subdir so the
    # destructive crash-recovery regression tests registered there
    # (hiom_crash_update_t*) attach to this project. Idempotent with the
    # enable_testing() in the HIOM TESTS block below.
    enable_testing()
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

    add_executable(cold_tier_test test/cold_tier_test.cpp)
    # Fault injection: compile the crash-point hooks (guarded by
    # VIPER_COLD_FAULT_INJECT in cold_tier.hpp) into this test ONLY — the
    # steady write path in every other TU carries no injection branch.
    target_compile_options(cold_tier_test PRIVATE -mclwb -pthread -DVIPER_COLD_FAULT_INJECT)
    if (CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(cold_tier_test PRIVATE -O3 -mtune=native)
    endif()
    target_link_libraries(cold_tier_test viper pthread)

    add_executable(hot_tier_test test/hot_tier_test.cpp)
    target_compile_options(hot_tier_test PRIVATE -mclwb -pthread)
    if (CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(hot_tier_test PRIVATE -O3 -mtune=native)
    endif()
    target_link_libraries(hot_tier_test viper pthread)

    add_executable(hiom_sigkill_test test/hiom_sigkill_test.cpp)
    target_compile_options(hiom_sigkill_test PRIVATE -mclwb -pthread)
    if (CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(hiom_sigkill_test PRIVATE -O3 -mtune=native)
    endif()
    target_link_libraries(hiom_sigkill_test viper pthread)

    add_executable(hiom_backpressure_test test/hiom_backpressure_test.cpp)
    target_compile_options(hiom_backpressure_test PRIVATE -mclwb -pthread)
    if (CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(hiom_backpressure_test PRIVATE -O3 -mtune=native)
    endif()
    target_link_libraries(hiom_backpressure_test viper pthread)

    # One-stop regression entry: `ctest` runs the whole HiOM suite.
    enable_testing()
    add_test(NAME cold_tier COMMAND cold_tier_test)
    add_test(NAME hot_tier COMMAND hot_tier_test)
    add_test(NAME hiom_integration COMMAND hiom_integration_test)
    # Bounded pending-ring backpressure: writes ~12M keys to /pmem0/hiom/
    # backpressure_test with a deliberately lazy flusher, forcing the residue
    # collision that used to abort. Not destructive (no SIGKILL) but writes a
    # multi-GiB pool and takes ~30-60s, so it runs in the default suite as the
    # guard that a fast producer + slow consumer completes instead of aborting.
    add_test(NAME hiom_backpressure COMMAND hiom_backpressure_test)
    # Destructive: wipes /pmem0/hiom/sigkill_test, 1 GiB pool, real
    # SIGKILLs. Opt-in so a plain `ctest` in an unknown environment never
    # runs it by accident; run manually or configure with
    # -DVIPER_BUILD_DESTRUCTIVE_TESTS=ON.
    if (${VIPER_BUILD_DESTRUCTIVE_TESTS})
        add_test(NAME hiom_sigkill COMMAND hiom_sigkill_test)
    endif()
endif()
