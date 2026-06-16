cmake_minimum_required(VERSION 3.16)
project(miniginx LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20: required for designated initializers, structured bindings on
# pairs/tuples (used by `auto [client, client_ip] = server.accept();`),
# and general modern-C++ idioms used throughout this codebase.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Phase 1: only the networking foundation sources are compiled.
# Later phases will add event/, http/, handler/, pool/, proxy/, cache/
# subdirectories and link them into this target.
# ---------------------------------------------------------------------------
add_executable(miniginx
    app/main.cpp
    src/net/ServerSocket.cpp
)

target_include_directories(miniginx PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)