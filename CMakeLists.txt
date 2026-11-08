cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AutoplayMod VERSION 1.0.0)

find_package(Geode REQUIRED)

add_geode_mod(${PROJECT_NAME}
    SOURCES
        src/main.cpp
)
