//
// Created by huangcheng on 2024/5/8.
//

#ifndef WEBSERVER_THREADPOOLMANAGER_H
#define WEBSERVER_THREADPOOLMANAGER_H

#include "threadpool.h"
#include "../timer/timerthread.h"

// 线程管理配置结构体
typedef struct ThreadPoolManagerConfig {
    ThreadPool *pool;               // 管理的线程池实例
    int maxThreads;                 // 最大线程数（这个主要是防止过度消耗系统资源）
    int minIdleThreads;             // 最小空闲线程数
    int releasePeriod;              // 空闲线程释放周期（以秒为单位）
    void *(*workFunction)(void *);  // 处理函数
    void *workFunctionArgs;         // 处理函数的参数
} ThreadPoolManagerConfig;

/**
 * 通用的线程池管理函数
 * @param arg 一个ThreadPoolManagerConfig实例的指针
 * @return NULL空指针
 */
void *ThreadPoolManagerThread(void *arg);



#endif //WEBSERVER_THREADPOOLMANAGER_H
