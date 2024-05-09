//
// Created by huangcheng on 2024/5/5.
//

#ifndef WEBSERVER_TCPPOOL_H
#define WEBSERVER_TCPPOOL_H

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "../list/list.h"
#include "../hashtable/hashtable.h"
#include "../timer/timerthread.h"
#include "../list/socketqueue.h"

//对这个连接池的操作必须是线程安全的

typedef struct tcpinfo {
    // 管理信息
    int timeout;                // 超时阈值
    int index;                  // 在等待数组中的下标，方便堆操作
    //内核链表嵌入结构体中，用链表来串联起每个结点，用于存储在内存中
    LIST_NODE node;
    // 处理信息
    int client_socket;          // 代表用户的TCP连接，在Linux中是一个文件描述符

} TCPINFO;

typedef struct tcppoll {
    // count为正在被管理的TCP连接数，size为数组大小
    volatile int count;
    // count变动性太强，要加上volatile，不得从缓存读取，size变动性不强
    int size;

    // 指针数组，在指针数组上建立数据结构，比在结构体数组数组上建立数据结构容易，因为指针更容易交换
    // 运用小顶堆思想改写（下标从1开始比较好计算），可以在pTCPINFO[0]处放置哨兵
    // 以timeout为排序键值
    TCPINFO **pTCPINFO;

    // 实际存储TCPINFO的存储结构是链表
    TCPINFO *head;

    // 文件描述符和指向TCPINFO的指针之间的哈希表，文件描述符为键，指针为值
    // 使用原因是监听TCP的时候只知道哪个文件描述符有动作，不知道对应的指针，所以用哈希表来方便查找，不然就要遍历了
    HashTable *pHashTable_fd_pTCPINFO;

    //这个连接池拥有的epoll实例
    int epoll_fd;

    pthread_mutex_t mutex;     // 线程互斥锁

} TCPPOOL;

/**
 * 创建一个已经初始化的连接池
 * @return 连接池指针
 */
TCPPOOL *CreateTCPPoll();

/**
 * 创建一个连接池可以管理的TCP连接信息
 * @param client_socket 代表用户的文件描述符
 * @param timeout 超时时间
 * @return TCP连接信息结构体指针
 */
TCPINFO *CreateTCPINFO(int client_socket, int timeout);

/**
 * 完全回收CreateTCPINFO创建的TCP连接信息
 * @param pTCPINFO CreateTCPINFO创建的TCP连接信息指针
 */
void FreeTCPINFO(TCPINFO *pTCPINFO);

/**
 * 将一个TCP连接信息交给连接池管理
 * @param pTCPPOOL 指向连接池的指针
 * @param pTCPINFO CreateTCPINFO创建的TCP连接信息指针
 * @return 成功返回1，失败返回0
 */
int AddTCPToTCPPool(TCPPOOL *pTCPPOOL, TCPINFO *pTCPINFO);

/**
 * 将一个本来由连接池管理的TCP连接从连接池中移除
 * @param pTCPPOOL 指向连接池的指针
 * @param pTCPINFO 指向TCP连接的指针
 * @return 返回所管理的TCP连接的Socket文件描述符
 */
int RemoveTCPFromTCPPool(TCPPOOL *pTCPPOOL, TCPINFO *pTCPINFO);

/**
 * 释放整个连接池所有资源
 * @param pTCPPOOL 指向连接池的指针
 */
void FreeTCPPool(TCPPOOL *pTCPPOOL);

/**
 * 输出TCP连接池的状态
 * @param pTCPPOOL
 */
void PrintTCPPoolStatus(TCPPOOL *pTCPPOOL);

/**
 * 连接池管理线程函数
 * @param arg 管理的连接池代码
 * @return
 */
void* tcppool_thread(void* arg);

#endif //WEBSERVER_TCPPOOL_H
