cmake_minimum_required(VERSION 3.24)
project(manta VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MANTA_BUILD_TESTS "Build the manta test suite" ON)
option(MANTA_LTO        "Enable link-time optimisation in Release" ON)
option(MANTA_SANITIZE   "Build with ASan+UBSan" OFF)
option(MANTA_STATIC     "Statically link the C++ runtime" OFF)

include(cmake/Warnings.cmake)

# --------------------------------------------------------------------------
# manta_core: everything except the CLI entry point, so tests can link it.
# --------------------------------------------------------------------------
file(GLOB_RECURSE MANTA_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM MANTA_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(manta_core STATIC ${MANTA_CORE_SOURCES})
target_include_directories(manta_core PUBLIC src)
target_compile_definitions(manta_core PUBLIC MANTA_VERSION="${PROJECT_VERSION}")
manta_set_warnings(manta_core)

# Determinism (spec 15.8): no __DATE__/__TIME__ leakage, reproducible paths.
target_compile_options(manta_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wdate-time>)

if(MANTA_SANITIZE)
    target_compile_options(manta_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(manta_core PUBLIC -fsanitize=address,undefined)
endif()

if(MANTA_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MANTA_SANITIZE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MANTA_IPO_OK OUTPUT MANTA_IPO_MSG)
    if(MANTA_IPO_OK)
        set_property(TARGET manta_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(manta_core PUBLIC Threads::Threads)

# --------------------------------------------------------------------------
# manta: the driver
# --------------------------------------------------------------------------
add_executable(manta src/main.cpp)
target_link_libraries(manta PRIVATE manta_core)
manta_set_warnings(manta)

if(MANTA_IPO_OK AND MANTA_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MANTA_SANITIZE)
    set_property(TARGET manta PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# A released binary is one file. On Windows that means no sidecar of MinGW
# DLLs; when cross-compiling for Linux it means not depending on the host's
# libstdc++ being new enough on whatever machine runs it.
if(MANTA_STATIC OR MINGW)
    target_link_options(manta PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS manta RUNTIME DESTINATION bin)
install(DIRECTORY schema/ DESTINATION share/manta/schema)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(MANTA_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()
