# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.15)
project(quiz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put the binary next to the sources so `./quiz` works the same way as the
# g++ one-liner documented in the README.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Quiz uses std::this_thread::sleep_for (fetch retry pause) and raw POSIX
# sockets. POSIX only — this will not build with MSVC on Windows.
find_package(Threads REQUIRED)
add_executable(quiz main.cpp)
target_link_libraries(quiz PRIVATE Threads::Threads)