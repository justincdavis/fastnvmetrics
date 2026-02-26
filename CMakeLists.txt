cmake_minimum_required(VERSION 3.18)
project(fastnvmetrics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static library for core (shared by _ext and tests)
add_library(fastnvmetrics_core STATIC
    src/config.cpp
    src/engine.cpp
)
target_include_directories(fastnvmetrics_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
set_target_properties(fastnvmetrics_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(fastnvmetrics_core PRIVATE -O3 -march=armv8-a)
target_compile_definitions(fastnvmetrics_core PRIVATE _GNU_SOURCE)
target_link_libraries(fastnvmetrics_core PRIVATE pthread)

# Python extension module
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)

execute_process(
    COMMAND ${Python_EXECUTABLE} -c "import nanobind; print(nanobind.cmake_dir())"
    OUTPUT_VARIABLE NB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_ext src/bindings.cpp)
target_link_libraries(_ext PRIVATE fastnvmetrics_core)

# Place .so in the fastnvmetrics/ Python package directory for editable installs
set_target_properties(_ext PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/fastnvmetrics")

install(TARGETS _ext LIBRARY DESTINATION fastnvmetrics)

# C++ tests (opt-in)
option(BUILD_TESTING "Build C++ tests" OFF)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(test_engine tests/test_engine.cpp)
    target_link_libraries(test_engine PRIVATE fastnvmetrics_core GTest::gtest_main)
    add_test(NAME test_engine COMMAND test_engine)
endif()
