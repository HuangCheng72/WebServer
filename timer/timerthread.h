//
// Created by huangcheng on 2024/5/7.
//

#ifndef WEBSERVER_TIMERTHREAD_H
#define WEBSERVER_TIMERTHREAD_H

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include "../list/list.h"

typedef struct timer_thread_control {
    pthread_t thread_id;    // 线程ID
    pthread_cond_t *cond;    // 条件变量
    pthread_mutex_t *mutex;  // 互斥锁
    LIST_NODE node;         // 链表节点
} TimerThreadControl;

/**
 * 注册线程到当前计时线程管理范围内
 * @param id 线程id
 * @return 当前被持有的计时线程管理的线程信息结构体指针
 */
TimerThreadControl *register_timer_thread(pthread_t id);

/**
 * 线程从当前计时线程管理范围内注销
 * @param pTimer_Ctl register_timer_thread返回的计时线程管理的线程信息结构体指针
 */
void unregister_timer_thread(TimerThreadControl *pTimer_Ctl);

/**
 * 计时线程
 * @param arg 传输空指针NULL
 * @return
 */
void *timer_thread(void *arg);



#endif //WEBSERVER_TIMERTHREAD_H
