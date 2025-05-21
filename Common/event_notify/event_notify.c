//
// Created by huangcheng on 2025/5/21.
//

#include "event_notify.h"
#include <sys/eventfd.h>

int eventfd_create() {
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

void eventfd_destroy(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int eventfd_notify(int fd) {
    uint64_t val = 1;
    return write(fd, &val, sizeof(val)) == sizeof(val) ? 0 : -1;
}

int eventfd_wait(int fd) {
    uint64_t val;
    return read(fd, &val, sizeof(val)) == sizeof(val) ? (int)val : -1;
}
