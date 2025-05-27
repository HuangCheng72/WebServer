cmake_minimum_required(VERSION 3.10)
project(WebServer)

set(CMAKE_C_STANDARD 11)

add_subdirectory(Common)
add_subdirectory(Daemon_Main)
add_subdirectory(Listener)
add_subdirectory(Manager)
add_subdirectory(Worker)
