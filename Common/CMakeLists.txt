add_library(Common include/list.h list/list.c include/transfer_fd.h transfer_utils/transfer_fd.c event_notify/event_notify.c include/event_notify.h)
target_include_directories(Common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
