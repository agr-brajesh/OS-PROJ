cmake_minimum_required(VERSION 3.16)
project(interactive_session_protector
    VERSION 1.0.0
    DESCRIPTION "Linux user-space daemon to dynamically protect interactive sessions using cgroups v2"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warning flags
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ------------------------------------------------------------------------------
# Core Library: isp_core
# ------------------------------------------------------------------------------
add_library(isp_core STATIC
    src/config.cpp
    src/logger.cpp
    src/session_detector.cpp
)

target_include_directories(isp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(isp_core PUBLIC Threads::Threads)

# ------------------------------------------------------------------------------
# Target 1: protector (Main daemon)
# ------------------------------------------------------------------------------
add_executable(protector
    src/protector_main.cpp
)
target_link_libraries(protector PRIVATE isp_core)

# ------------------------------------------------------------------------------
# Target 2: fake_call (Synthetic benchmark workload app)
# ------------------------------------------------------------------------------
add_executable(fake_call
    src/fake_call_main.cpp
)
target_link_libraries(fake_call PRIVATE isp_core)

# ------------------------------------------------------------------------------
# Target 3: unit_tests (Unit test suite)
# ------------------------------------------------------------------------------
add_executable(unit_tests
    tests/test_main.cpp
    tests/test_config.cpp
    tests/test_session_detector.cpp
)
target_include_directories(unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(unit_tests PRIVATE isp_core)

# Enable CTest integration
enable_testing()
add_test(NAME ConfigUnitTests COMMAND unit_tests)
