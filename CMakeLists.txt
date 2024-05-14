cmake_minimum_required(VERSION 3.10)
project(WebServer)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread") # 添加这行确保编译器和链接器都使用-pthread标志

# 查找线程库
find_package(Threads REQUIRED)

# 指定源文件
add_executable(WebServer main.c list/list.h list/list.c timer/timerthread.h timer/timerthread.c list/socketqueue.c list/socketqueue.h hashtable/hashtable.c hashtable/hashtable.h tcppool/tcppool.c tcppool/tcppool.h threadpool/threadpool.c threadpool/threadpool.h threadpool/threadpoolmanager.c threadpool/threadpoolmanager.h list/RequestQueue.c list/RequestQueue.h socketserver/socketserver.c socketserver/socketserver.h filepool/filepool.c filepool/filepool.h)

# 链接线程库和实时库
target_link_libraries(WebServer ${CMAKE_THREAD_LIBS_INIT} rt)
