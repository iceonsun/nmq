cmake_minimum_required(VERSION 3.8)
project(sockNM)

set(CMAKE_CXX_STANDARD 11)

set(SRC nmq.c dlist.c enc.c util.c)
add_executable(test_nmq ${SRC} test/test.cpp)
add_library(nmq ${SRC})