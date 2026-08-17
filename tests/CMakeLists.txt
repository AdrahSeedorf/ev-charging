include(FetchContent)

# Prefer a system Catch2 when one is installed, otherwise fetch it. The first
# configure therefore needs network access; subsequent ones do not.
FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
        FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)

add_executable(evnet_tests
        test_network.cpp
        test_router.cpp)

target_link_libraries(evnet_tests PRIVATE evnet_core Catch2::Catch2WithMain)

# Dataset tests read the shipped CSVs, so they need the repository root.
target_compile_definitions(evnet_tests PRIVATE
        EVNET_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")

if(NOT MSVC)
    target_compile_options(evnet_tests PRIVATE -Wall -Wextra)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(evnet_tests)
