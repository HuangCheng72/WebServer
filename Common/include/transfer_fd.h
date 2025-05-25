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

/**
 * 状态结构体
 */
typedef struct {
    int module;                 // 1=Listener, 2=Manager, 3=Worker，靠自己区分
    int pid;                    // 模块PID

    int args[14];               // 各模块自己自由使用的参数，看参数和参数之间的约定
} StatusMessage;

/**
 * 发送状态结构体
 * @param sock 发送目标socket fd
 * @param status 状态结构体指针
 * @return
 */
int send_status_message(int sock, const StatusMessage *status);

/**
 * 接收状态结构体
 * @param sock 接收自哪个socket
 * @param status_out 存储的状态结构体指针
 * @return
 */
int recv_status_message(int sock, StatusMessage *status_out);

#endif //WEBSERVER_TRANSFER_FD_H
