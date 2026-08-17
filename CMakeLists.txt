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

# The core is a library rather than a pile of headers. Both legacy projects put
# every method body in a header, which compiled only because each had a single
# translation unit -- adding a second .cpp that included them would have produced
# duplicate-symbol link errors.
add_library(evnet_core STATIC
        src/csv.cpp)

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
