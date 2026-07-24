cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER clang++)

project(raygamescpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "CPP Compiler: ${CMAKE_CXX_COMPILER}")


# Define raylib location
set(RAYLIB_DIR ${CMAKE_BINARY_DIR}/raylib)
set(RAYLIB_LIB ${RAYLIB_DIR}/lib/libraylib.so)
set(RAYLIB_INCLUDE_DIR ${RAYLIB_DIR}/include)

# Verify raylib exists
if(NOT EXISTS ${RAYLIB_LIB})
    message(FATAL_ERROR "raylib not found at: ${RAYLIB_LIB}")
endif()


# Default to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Debug configuration with sanitizers
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address -fsanitize=undefined")

# Release configuration
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")


set(SRC_FILES
)

add_executable(main
    src/main.cpp
    ${SRC_FILES}
)

# Include raylib headers, plus ours
target_include_directories(main PRIVATE
    include
    include/ecs
    include/ecs/utils
    include/ecs/components
    include/ecs/systems
    ${RAYLIB_INCLUDE_DIR}
)

# Link raylib
target_link_libraries(main PRIVATE ${RAYLIB_LIB})



