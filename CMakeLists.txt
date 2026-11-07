cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)

# If GEODE_SDK environment variable is set, add it to prefix path
if (DEFINED ENV{GEODE_SDK})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{GEODE_SDK}")
endif()

project(AutoplayMod)

find_package(Geode REQUIRED)

add_geode_mod(indonesiangamer777.autoplay
    SOURCES
        src/main.cpp
)
