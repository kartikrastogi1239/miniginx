cmake_minimum_required(VERSION 3.16)
project(miniginx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(miniginx
    app/main.cpp
    src/net/ServerSocket.cpp
    src/net/Connection.cpp
)

target_include_directories(miniginx PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)