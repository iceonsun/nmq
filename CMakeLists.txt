cmake_minimum_required(VERSION 3.8)
project(sockNM)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES nmq.h types.h nmq.c dlist.h dlist.c enc.h enc.c util.h util.c test.h  test.cpp)
#set(SOURCE_FILES test_client.c nmq.h types.h nmq.c dlist.h dlist.c enc.h enc.c util.h util.c test_server.c test_util.h test.cpp test.h)
add_executable(sockNM ${SOURCE_FILES})

set(LIB_NMQ_FILES nmq.h types.h nmq.c dlist.h dlist.c enc.h enc.c util.h util.c)
add_library(nmq SHARED ${LIB_NMQ_FILES})