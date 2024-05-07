//
// Created by huangcheng on 2024/5/6.
//

#include "timerthread.h"

// 用链表记录订阅了计时信息的线程信息
LIST_HEAD(timerthread_list_head);
// 只用于计时管理线程的锁，在名字上区分
pthread_mutex_t timer_global_lock = PTHREAD_MUTEX_INITIALIZER;

TimerThreadControl *register_timer_thread(pthread_t id) {
    TimerThreadControl *ctrl = malloc(sizeof(TimerThreadControl));
    if (!ctrl) {
        perror("Failed to allocate thread control");
        return ctrl;
    }
    ctrl->thread_id = id;
    //申请条件变量和锁
    ctrl->cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    ctrl->mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_cond_init(ctrl->cond, NULL);
    pthread_mutex_init(ctrl->mutex, NULL);

    pthread_mutex_lock(&timer_global_lock);
    list_add_tail(&ctrl->node, &timerthread_list_head);
    pthread_mutex_unlock(&timer_global_lock);
    return ctrl;
}

void unregister_timer_thread(TimerThreadControl *pTimer_Ctl) {
    pthread_mutex_lock(&timer_global_lock);
    list_del(&pTimer_Ctl->node);
    pthread_cond_destroy(pTimer_Ctl->cond);
    pthread_mutex_destroy(pTimer_Ctl->mutex);
    free(pTimer_Ctl->cond);
    free(pTimer_Ctl->mutex);
    free(pTimer_Ctl);
    pthread_mutex_unlock(&timer_global_lock);
}

void *timer_thread(void *arg) {
    struct timespec spec;
    long long last_time, current_time;

    // 获取初始时间
    clock_gettime(CLOCK_MONOTONIC, &spec);
    last_time = spec.tv_sec * 1000000000LL + spec.tv_nsec;

    while (1) {
        // 不断获取当前时间
        clock_gettime(CLOCK_MONOTONIC, &spec);
        current_time = spec.tv_sec * 1000000000LL + spec.tv_nsec;

        // 检查是否已经过去了一秒钟（1秒 = 1,000,000,000纳秒）
        if (current_time - last_time >= 1000000000LL) {
            // 更新上次记录的时间
            last_time = current_time;

            // 向所有注册的线程发出通知
            pthread_mutex_lock(&timer_global_lock);
            LIST_NODE *pos;
            list_for_each(pos, &timerthread_list_head) {
                TimerThreadControl *ctrl = list_entry(pos, TimerThreadControl, node);
                pthread_cond_signal(ctrl->cond);  // 发送通知
            }
            pthread_mutex_unlock(&timer_global_lock);
        }
    }
}
