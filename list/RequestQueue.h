//
// Created by huangcheng on 2024/5/8.
//

#ifndef WEBSERVER_REQUESTQUEUE_H
#define WEBSERVER_REQUESTQUEUE_H

#include "../list/list.h"
#include <string.h>

// 这个队列是线程不安全的，只能同时在单个线程中进行操作

// 定义请求数据结构
typedef struct request_data {
    LIST_NODE list;      // 链表节点，用于插入到队列中
    char *data;          // 存储请求数据的指针
    int length;          // 请求数据的长度
} RequestData;

/**
 * 创建一个空白的请求数据
 * @return 创建的空白请求数据的指针
 */
RequestData *CreateRequestData();

/**
 * 完全回收CreateRequestData函数创建的请求数据
 * @param pData CreateRequestData创建的请求数据指针
 */
void FreeRequestData(RequestData *pData);

// 定义请求队列
typedef struct request_queue {
    LIST_NODE head;      // 链表头，用于管理队列
} RequestQueue;

/**
 * 创建一个完整的请求队列
 * @return 创建的空白的请求队列的指针
 */
RequestQueue *CreateRequestQueue();

/**
 * 入队操作，将请求数据添加到队列中
 * @param pRequestQueue 指向请求队列的指针
 * @param data 请求数据的字符串
 * @param length 请求数据的长度
 */
void AddToRequestQueue(RequestQueue *pRequestQueue, char *data, int length);

/**
 * 出队操作，从队列中获取一个请求数据
 * @param pRequestQueue 指向请求队列的指针
 * @return 指向第一个（队头）请求数据的指针
 */
RequestData *GetFromRequestQueue(RequestQueue *pRequestQueue);

/**
 * 检查队列是否为空
 * @param pRequestQueue 指向请求队列的指针
 * @return 为空返回1，非空返回0
 */
int IsRequestQueueEmpty(RequestQueue *pRequestQueue);


#endif //WEBSERVER_REQUESTQUEUE_H
