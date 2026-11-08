cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 20)

project(AutoplayMod VERSION 1.0.0 LANGUAGES CXX)

find_package(Geode REQUIRED)

add_geode_mod(indonesiangamer777.autoplay
    SOURCES
        src/main.cpp
)
