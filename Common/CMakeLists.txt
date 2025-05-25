add_library(Common include/list.h list/list.c include/transfer_fd.h transfer_utils/transfer_fd.c event_notify/event_notify.c include/event_notify.h)
target_include_directories(Common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 公共组件库增加查找链接线程库，统一支持
find_package(Threads REQUIRED)
target_link_libraries(Common PUBLIC Threads::Threads)
