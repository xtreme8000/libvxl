cmake_minimum_required(VERSION 2.8)
project(vxl C)

set(CMAKE_C_STANDARD 99)

add_compile_options(-Wall -Wextra -pedantic -std=c99 -Ofast)

add_library(vxl STATIC libvxl.c libvxl.h)
