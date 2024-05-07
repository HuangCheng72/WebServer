//
// Created by huangcheng on 2024/5/8.
//

#include "threadpoolmanager.h"

// 通用的线程管理函数
void *ThreadPoolManagerThread(void *arg) {
    if(!arg) {
        return NULL;
    }

    ThreadPoolManagerConfig *config = (ThreadPoolManagerConfig *)arg;

    // 注册当前线程到计时管理线程
    TimerThreadControl *timer_ctrl = register_timer_thread(pthread_self());
    // 存在依赖关系，不得不重试
    while (!timer_ctrl) {
        timer_ctrl = register_timer_thread(pthread_self());
        if (!timer_ctrl) {
            sleep(1);  // 睡眠1秒后重试
        }
    }

    int count = config->releasePeriod;

    // 确保初始线程池空闲线程数满足最小空闲线程数要求
    while(config->pool->idlecount < config->minIdleThreads) {
        AddThreadToIdleQueue(config->pool, CreateThread(config->pool, config->workFunction, config->workFunctionArgs));
    }

    while (1) {

        // 等待计时器线程的信号，阻塞时间极短，可以认为是非阻塞方式
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // 获取当前时间

        pthread_mutex_lock(timer_ctrl->mutex);
        // 设置超时时间为当前时间，即不等待
        int wait_result = pthread_cond_timedwait(timer_ctrl->cond, timer_ctrl->mutex, &now);
        pthread_mutex_unlock(timer_ctrl->mutex);

        if(wait_result == 0) {
            count--;
        }
        if(count == 0) {
            // 因为ShrinkThreadPool这东西用了线程池锁保证安全，所以只能离开锁的作用域使用
            ShrinkThreadPool(config->pool, 10);
            // 规定时间销毁一次
            count = config->releasePeriod;
        }

        if (config->pool->pShouldAssignWork()) {
            pthread_mutex_lock(&config->pool->lock);
            if (!list_empty(&config->pool->idle_queue)) {
                LIST_NODE *node = config->pool->idle_queue.next;
                Thread *thread = list_entry(node, Thread, node);
                list_del(node);
                thread->is_working = 1;
                list_add_tail(node, &config->pool->work_queue);
                config->pool->activecount++;
                pthread_cond_signal(&thread->cond);
            }
            pthread_mutex_unlock(&config->pool->lock);
            ExpandThreadPool(config->pool, config->maxThreads, config->workFunction, config->workFunctionArgs);
        }

        // 回收已完成工作的线程
        pthread_mutex_lock(&config->pool->lock);
        LIST_NODE *pos, *n;
        list_for_each_safe(pos, n, &config->pool->work_queue) {
            Thread *thread = list_entry(pos, Thread, node);
            pthread_mutex_lock(&thread->mutex);
            if (!thread->is_working) {
                list_del(pos);
                config->pool->activecount--;
                list_add_tail(pos, &config->pool->idle_queue);
                config->pool->idlecount++;
            }
            pthread_mutex_unlock(&thread->mutex);
        }
        pthread_mutex_unlock(&config->pool->lock);
    }

    // 解除订阅计时线程
    unregister_timer_thread(timer_ctrl);
    destroyThreadPool(config->pool);

    return NULL;
}