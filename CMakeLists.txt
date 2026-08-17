cmake_minimum_required(VERSION 3.16)

project(ev-network-toolkit
        VERSION 0.1.0
        DESCRIPTION "Capacitated network routing, congestion and facility siting"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(EVNET_WERROR "Treat warnings as errors" OFF)
option(EVNET_BUILD_TESTS "Build the test suite" ON)

# The core is a library rather than a pile of headers. Both legacy projects put
# every method body in a header, which compiled only because each had a single
# translation unit -- adding a second .cpp that included them would have produced
# duplicate-symbol link errors.
add_library(evnet_core STATIC
        src/candidates.cpp
        src/csv.cpp
        src/network.cpp
        src/policy.cpp
        src/router.cpp
        src/simulation.cpp
        src/siting.cpp
        src/station_runtime.cpp
        src/station_state.cpp)

target_include_directories(evnet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(evnet_core PRIVATE /W4)
    if(EVNET_WERROR)
        target_compile_options(evnet_core PRIVATE /WX)
    endif()
else()
    target_compile_options(evnet_core PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(EVNET_WERROR)
        target_compile_options(evnet_core PRIVATE -Werror)
    endif()
endif()

if(EVNET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Convenience: regenerate the datasets from their raw sources.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(datasets
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_datasets.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Rebuilding data/ from raw legacy sources")
endif()
