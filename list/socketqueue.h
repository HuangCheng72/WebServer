//
// Created by huangcheng on 2024/5/7.
//

#ifndef WEBSERVER_SOCKETQUEUE_H
#define WEBSERVER_SOCKETQUEUE_H

#include "list.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    LIST_NODE node;
    int fd;
} SocketTask;

typedef struct {
    LIST_NODE head;
    pthread_mutex_t lock;
} SocketTaskQueue;

/**
 * 创建一个已经初始化好的SocketTaskQueue
 * @return 指向这个SocketTaskQueue的指针
 */
SocketTaskQueue* CreatedSocketTaskQueue();

/**
 * 完全释放一个SocketTaskQueue所有资源
 * @param queue 指向SocketTaskQueue的指针
 */
void FreeSocketQueue(SocketTaskQueue *queue);

/**
 * 添加一个socket的文件描述符到队列（队尾）
 * @param queue 指向SocketTaskQueue的指针
 * @param fd socket的文件描述符
 */
void AddToSocketQueue(SocketTaskQueue *queue, int fd);

/**
 * 从队列中取出一个socket的文件描述符（队头）
 * @param queue 指向SocketTaskQueue的指针
 * @return 成功返回socket的文件描述符，失败返回-1
 */
int GetFromSocketQueue(SocketTaskQueue *queue);

/**
 * 检查队列是否为空
 * @param queue 指向SocketTaskQueue的指针
 * @return 为空返回1，非空返回0
 */
int IsSocketQueueEmpty(SocketTaskQueue *queue);

#endif //WEBSERVER_SOCKETQUEUE_H
