//
// Created by huangcheng on 2024/5/9.
//

#ifndef WEBSERVER_SOCKETSERVER_H
#define WEBSERVER_SOCKETSERVER_H

#include "../list/socketqueue.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    int port;                       // 监听端口
    int max_backlog;                // 允许系统为该监听套接字挂起的未完成连接的最大数量，即listen函数的第二个参数
    // 后续可以继续添加其他配置项，如日志级别、超时时间等

    volatile int server_running;    // 线程状态：0 停止，1 运行
    pthread_mutex_t state_mutex;    // 互斥锁保护线程状态
} ServerConfig;

/**
 * 初始化ServerConfig中的一些不该由用户赋值的东西
 * @param pConfig 指向一个ServerConfig结构体的指针
 */
void InitServerConfig(ServerConfig *pConfig);

/**
 * 完全销毁一个ServerConfig实例
 * @param pConfig pConfig 指向一个ServerConfig结构体的指针
 */
void FreeServerConfig(ServerConfig *pConfig);

/**
 * 用于本程序的网络连接管理的线程函数
 * @param arg 指向一个ServerConfig结构体的指针
 * @return
 */
void *SocketServerThread(void *arg);

/**
 * 输出网络连接管理线程当前状态
 * @param config 指向一个ServerConfig结构体的指针
 */
void PrintSocketServerStatus(ServerConfig *pConfig);


#endif //WEBSERVER_SOCKETSERVER_H
