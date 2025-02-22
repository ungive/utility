cmake_minimum_required(VERSION 3.8)
project(ungive_utility)

option(UNGIVE_UTILITY_BUILD_TESTS "Build tests" OFF)

add_library(ungive_utility INTERFACE)
target_include_directories(ungive_utility INTERFACE ./include)

set_property(TARGET ungive_utility PROPERTY CXX_STANDARD 11)
set_property(TARGET ungive_utility PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(UNGIVE_UTILITY_BUILD_TESTS)
    # Prevent overriding the parent project's compiler/linker settings (win)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/gtest EXCLUDE_FROM_ALL)
    include(GoogleTest)
    enable_testing()
    set(TEST_SOURCES test/atomic_test.cpp)
    list(APPEND TEST_SOURCES test/detail/atomic_zero_counter_test.cpp)

    add_executable(ungive_utility_test ${TEST_SOURCES})
    target_compile_definitions(ungive_utility_test PRIVATE UNGIVE_UTILITY_TEST)
    target_link_libraries(ungive_utility_test ungive_utility)
    target_link_libraries(ungive_utility_test GTest::gmock_main)
    target_include_directories(
        ungive_utility_test PRIVATE ${gtest_SOURCE_DIR}/include
                                    ${gtest_SOURCE_DIR})

    set_property(TARGET ungive_utility_test PROPERTY CXX_STANDARD 11)
    set_property(TARGET ungive_utility_test PROPERTY CMAKE_CXX_STANDARD_REQUIRED
                                                     ON)

    add_test(ungive_utility_test ungive_utility_test)
    gtest_discover_tests(ungive_utility_test)
endif()
