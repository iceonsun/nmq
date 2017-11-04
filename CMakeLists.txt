cmake_minimum_required(VERSION 3.8)
project(sockNM)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES nmq.h types.h nmq.c dlist.h dlist.c enc.h enc.c util.h util.c test.h  test.cpp)
#set(SOURCE_FILES test_client.c nmq.h types.h nmq.c dlist.h dlist.c enc.h enc.c util.h util.c test_server.c test_util.h test.cpp test.h)
add_executable(sockNM ${SOURCE_FILES})

### add libuv. g++ -luv
# /usr/local/lib is path for libuv.dylib  on mac or so on other platforms
find_library(UV_PATH uv /usr/local/lib NO_DEFAULT_PATH)
link_libraries($UV_PATH)
target_link_libraries(sockNM uv)