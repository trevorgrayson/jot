cmake_minimum_required(VERSION 3.23)
project(jot)

set(CMAKE_CXX_STANDARD 14)

add_executable(jot
        _/mqtt/src/main.cpp
        src/main.cpp)

