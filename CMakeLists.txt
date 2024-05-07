cmake_minimum_required(VERSION 3.10)
project(WebServer)

set(CMAKE_C_STANDARD 11)

# 查找线程库
find_package(Threads REQUIRED)

# 指定源文件
add_executable(WebServer main.c list/list.h list/list.c timer/timerthread.h timer/timerthread.c)

# 链接线程库
target_link_libraries(WebServer ${CMAKE_THREAD_LIBS_INIT} rt)
