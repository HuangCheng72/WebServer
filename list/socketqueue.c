//
// Created by huangcheng on 2024/5/7.
//

#include "socketqueue.h"

// 创建并完全初始化队列
SocketTaskQueue* CreatedSocketTaskQueue() {
    SocketTaskQueue *queue = (SocketTaskQueue *)malloc(sizeof(SocketTaskQueue));
    if (queue) {
        init_list_node(&queue->head);
        pthread_mutex_init(&queue->lock, NULL);
    }
    return queue;
}

// 完全清理队列所有资源
void FreeSocketQueue(SocketTaskQueue *queue) {
    if (queue != NULL) {
        pthread_mutex_lock(&queue->lock);
        // 清理所有节点
        LIST_NODE *current;
        LIST_NODE *temp;
        list_for_each_safe(current, temp, &queue->head) {
            list_del(current);
            SocketTask *task = list_entry(current, SocketTask, node);
            free(task);
        }
        pthread_mutex_unlock(&queue->lock);
        // 销毁锁
        pthread_mutex_destroy(&queue->lock);
        // 释放队列结构体
        free(queue);
    }
}

// 添加socket到队列尾部
void AddToSocketQueue(SocketTaskQueue *queue, int fd) {
    if (!queue) return;
    SocketTask *new_task = (SocketTask *)malloc(sizeof(SocketTask));
    new_task->fd = fd;
    pthread_mutex_lock(&queue->lock);
    list_add_tail(&new_task->node, &queue->head);
    pthread_mutex_unlock(&queue->lock);
}

// 从队列头部获取socket，如果队列为空则返回-1
int GetFromSocketQueue(SocketTaskQueue *queue) {
    //队列根本不存在，当然为空
    if (!queue) return -1;
    pthread_mutex_lock(&queue->lock);
    if (list_empty(&queue->head)) {
        pthread_mutex_unlock(&queue->lock);
        return -1;
    }
    LIST_NODE *first = queue->head.next;
    list_del(first);
    pthread_mutex_unlock(&queue->lock);
    SocketTask *task = list_entry(first, SocketTask, node);
    int fd = task->fd;
    free(task);
    return fd;
}

// 检查队列是否为空
int IsSocketQueueEmpty(SocketTaskQueue *queue) {
    //队列根本不存在，当然为空
    if (!queue) return 1;
    pthread_mutex_lock(&queue->lock);
    int empty = list_empty(&queue->head);
    pthread_mutex_unlock(&queue->lock);
    return empty;
}
