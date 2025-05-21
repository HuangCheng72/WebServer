//
// Created by huangcheng on 2025/5/21.
//

/**
 * 提供已封装的用于进程间通信的 UNIX 域 （本地）Socket对的操作函数
 */

#ifndef WEBSERVER_TRANSFER_FD_H
#define WEBSERVER_TRANSFER_FD_H

#include <unistd.h>
#include <sys/types.h>

// 文件描述符传递通信通道对
typedef struct {
    int sock1;
    int sock2;
} SOCKETPAIR;

/**
 * 创建一对用于进程间通信的本地 socket。
 * @param pair 指向 FDPAIR 的指针，成功后填入两个文件描述符
 * @return 成功返回0，失败返回-1
 */
int socketpair_create(SOCKETPAIR *pair);

/**
 * 销毁一对用于文件描述符传递的本地 socket。
 * @param pair 指向 FDPAIR 的指针
 */
void socketpair_destroy(SOCKETPAIR *pair);

/**
 * 绑定套接字到指定的文件路径
 * @param sock 套接字描述符
 * @param path 要绑定的文件路径
 * @return 成功返回0，失败返回-1
 */
int socket_bind_to_path(int sock, const char *path);

/**
 * 解绑套接字的文件路径
 * @param path 要解绑的文件路径
 * @return 成功返回0，失败返回-1
 */
int socket_unlink_path(const char *path);

/**
 * 通过本地 socket 发送一个文件描述符
 * @param sock 用于发送的本地 socket fd
 * @param fd 要发送的目标文件描述符
 * @return 成功返回0，失败返回-1
 */
int fd_send(int sock, int fd);

/**
 * 从本地 socket 接收一个文件描述符
 * @param sock 用于接收的本地 socket fd
 * @return 接收到的 fd，失败返回-1
 */
int fd_recv(int sock);

#endif //WEBSERVER_TRANSFER_FD_H
