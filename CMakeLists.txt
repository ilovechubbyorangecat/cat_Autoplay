cmake_minimum_required(VERSION 3.21)
project(AutoplayMod)

set(CMAKE_CXX_STANDARD 20)

find_package(Geode REQUIRED)

add_geode_mod(indonesiangamer777.autoplay
    SOURCES
        src/main.cpp
)
