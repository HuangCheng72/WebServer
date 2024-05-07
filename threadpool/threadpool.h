//
// Created by huangcheng on 2024/5/7.
//

#ifndef WEBSERVER_THREADPOOL_H
#define WEBSERVER_THREADPOOL_H

#include <pthread.h>
#include <unistd.h>
#include "../list/list.h"

// 定义线程结构体
typedef struct {
    LIST_NODE node;                 // 链表节点，用于插入线程池的管理队列
    pthread_t thread;               // 线程标识
    pthread_cond_t cond;            // 条件变量
    pthread_mutex_t mutex;          // 与条件变量配合使用的互斥锁
    int is_working;                 // 是否正在工作
    void *(*pFunction)(void *);     // 线程执行的函数
    void *pArgs;                    // 线程执行函数可能需要的参数，指针型方便打包，怎么拆包在函数里面可以任意，如果没有就设置为NULL
} Thread;

// 定义线程池结构体
typedef struct {
    LIST_NODE work_queue;               // 工作队列
    LIST_NODE idle_queue;               // 空闲队列
    pthread_mutex_t lock;               // 线程池锁
    int (*pShouldAssignWork)();         // 函数指针，用于判断是否该让线程分配工作的条件，必须是一个不带参数的函数，看情况你自己判断
    int activecount;                    // 活跃线程数
    int idlecount;                      // 空闲线程数

} ThreadPool;

/**
 * 创建并初始化一个线程池
 * @param pShouldAssignWork 判断线程池什么时候唤醒空闲线程进行工作的条件，必须是必须是一个不带参数且返回整数值的函数，具体操作看情况适配
 * @return 指向线程池的指针
 */
ThreadPool *CreateThreadPool(int (*pShouldAssignWork)());

/**
 * 线程池管理的线程，实际上做什么工作都是用户指定的
 * @param arg 传递CreateThread返回的线程信息Thread的指针，用来开启工作
 * @return 返回NULL
 */

void *ThreadFunctionWrapper(void *arg);

/**
 * 创建并初始化一个可以被线程池管理的线程信息
 * @param pool 指向线程池的指针
 * @param pFunction 线程使用的工作函数
 * @param pArgs 工作函数所需的参数，一并传递，没参数就传递NULL
 * @return 线程信息Thread的指针
 */
Thread *CreateThread(ThreadPool *pool, void *(*pFunction)(void *), void *pArgs);

/**
 * 将线程添加到线程池的空闲队列中
 * @param pool 指向线程池的指针
 * @param thread CreateThread返回的线程信息Thread的指针
 */
void AddThreadToIdleQueue(ThreadPool *pool, Thread *thread);


/**
 * 线程安全扩展线程池，必须离开线程池锁作用域使用
 * @param pool 指向线程池的指针
 * @param pFunction 线程使用的工作函数
 * @param pArgs 工作函数所需的参数，一并传递，没参数就传递NULL
 */
void ExpandThreadPool(ThreadPool *pool, void *(*pFunction)(void *), void *pArgs);

/**
 * 线程安全缩减线程池，必须离开线程池锁作用域使用
 * @param pool 指向线程池的指针
 * @param minsize 最少的空闲线程数冗余（最起码这么多线程要留在线程池里面随时待命）
 */
void ShrinkThreadPool(ThreadPool *pool, int minsize);

/**
 * 销毁整个线程池
 * @param pool 指向线程池的指针
 */
void destroyThreadPool(ThreadPool *pool);


#endif //WEBSERVER_THREADPOOL_H
