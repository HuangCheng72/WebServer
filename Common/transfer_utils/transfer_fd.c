//
// Created by huangcheng on 2025/5/21.
//

#include "transfer_fd.h"
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#define CONTROL_LEN CMSG_LEN(sizeof(int))

// 创建本地 socket 对
int socketpair_create(SOCKETPAIR *pair) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        return -1;
    }
    pair->sock1 = sv[0];
    pair->sock2 = sv[1];
    return 0;
}

// 销毁本地 socket 对
void socketpair_destroy(SOCKETPAIR *pair) {
    if (pair->sock1 >= 0) {
        close(pair->sock1);
        pair->sock1 = -1;
    }
    if (pair->sock2 >= 0) {
        close(pair->sock2);
        pair->sock2 = -1;
    }
}

// 发送文件描述符
int fd_send(int sock, int fd) {
    struct msghdr msg;
    struct iovec iov;
    char dummy_data[1] = {0};
    char control_buf[CONTROL_LEN];

    memset(&msg, 0, sizeof(msg));
    memset(control_buf, 0, sizeof(control_buf));

    iov.iov_base = dummy_data;
    iov.iov_len = sizeof(dummy_data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control_buf;
    msg.msg_controllen = CONTROL_LEN;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (sendmsg(sock, &msg, 0) < 0) {
        return -1;
    }
    return 0;
}

// 接收文件描述符
int fd_recv(int sock) {
    struct msghdr msg;
    struct iovec iov;
    char dummy_buf[1];
    char control_buf[CONTROL_LEN];

    memset(&msg, 0, sizeof(msg));
    memset(control_buf, 0, sizeof(control_buf));

    iov.iov_base = dummy_buf;
    iov.iov_len = sizeof(dummy_buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control_buf;
    msg.msg_controllen = CONTROL_LEN;

    if (recvmsg(sock, &msg, 0) < 0) {
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

int send_status_message(int sock, const StatusMessage *status) {
    struct msghdr msg = {0};
    struct iovec iov;

    iov.iov_base = (void *)status;
    iov.iov_len = sizeof(StatusMessage);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // 不带控制消息 -> 发送的是状态结构体
    return sendmsg(sock, &msg, 0);
}

int recv_status_message(int sock, StatusMessage *status_out) {
    struct msghdr msg = {0};
    struct iovec iov;
    char control_buf[CONTROL_LEN]; // 需要提供但我们会判断不使用它

    iov.iov_base = status_out;
    iov.iov_len = sizeof(StatusMessage);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control_buf;
    msg.msg_controllen = CONTROL_LEN;

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg) {
        // 是FD而不是StatusMessage，接收错误
        memset(status_out, 0, sizeof(StatusMessage)); // 防止误用残留数据
        return -2;
    }

    return 0;
}
