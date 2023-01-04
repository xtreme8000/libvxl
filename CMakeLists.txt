cmake_minimum_required(VERSION 2.8)
project(vxl C)

set(CMAKE_C_STANDARD 99)

if (NOT MSVC)
add_compile_options(-Wall -Wextra -pedantic -std=c99 -Ofast)
endif()

add_library(vxl STATIC libvxl.c)

target_include_directories(vxl PUBLIC .)
