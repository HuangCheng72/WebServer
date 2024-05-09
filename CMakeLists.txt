cmake_minimum_required(VERSION 3.10)
project(WebServer)

set(CMAKE_C_STANDARD 11)

# 查找线程库
find_package(Threads REQUIRED)

# 指定源文件
add_executable(WebServer main.c list/list.h list/list.c timer/timerthread.h timer/timerthread.c list/socketqueue.c list/socketqueue.h hashtable/hashtable.c hashtable/hashtable.h tcppool/tcppool.c tcppool/tcppool.h threadpool/threadpool.c threadpool/threadpool.h threadpool/threadpoolmanager.c threadpool/threadpoolmanager.h list/RequestQueue.c list/RequestQueue.h)

# 链接线程库
target_link_libraries(WebServer ${CMAKE_THREAD_LIBS_INIT} rt)
