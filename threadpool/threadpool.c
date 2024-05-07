//
// Created by huangcheng on 2024/5/7.
//

#include "threadpool.h"

// 创建线程池
ThreadPool *CreateThreadPool(int (*pShouldAssignWork)()) {
    ThreadPool *pool = (ThreadPool *)malloc(sizeof(ThreadPool));
    if (!pool) {
        return NULL;
    }

    init_list_node(&pool->work_queue);
    init_list_node(&pool->idle_queue);
    pthread_mutex_init(&pool->lock, NULL);
    pool->pShouldAssignWork = pShouldAssignWork;
    // 新建的线程池，什么都没有
    pool->activecount = 0;
    pool->idlecount = 0;

    return pool;
}

// 给具体的工作函数适配（装饰器的作用，这样我们的线程就都能管理了）
void *ThreadFunctionWrapper(void *arg) {
    Thread *pt = (Thread *)arg;
    while (1) {
        pthread_mutex_lock(&pt->mutex);
        while (!pt->is_working) {  // 当没有工作时，线程进入等待状态
            pthread_cond_wait(&pt->cond, &pt->mutex);
        }
        pthread_mutex_unlock(&pt->mutex);

        if (pt->pFunction) {  // 检查工作函数是否存在并执行
            pt->pFunction(pt->pArgs);
        }
        // 标记工作完成并通知可能等待的线程池管理线程
        pthread_mutex_lock(&pt->mutex);
        pt->is_working = 0;  // 标记线程工作完成
        pthread_mutex_unlock(&pt->mutex);
    }
    return NULL;
}

// 创建并初始化线程池中的线程
Thread *CreateThread(ThreadPool *pool, void *(*pFunction)(void *), void *pArgs) {
    Thread *thread = (Thread *)malloc(sizeof(Thread));
    if(!thread) {
        return NULL;
    }
    thread->is_working = 0;
    thread->pFunction = pFunction;
    thread->pArgs = pArgs;
    init_list_node(&(thread->node));
    pthread_mutex_init(&thread->mutex, NULL);  // 初始化互斥锁
    pthread_cond_init(&thread->cond, NULL);    // 初始化条件变量

    pthread_create(&thread->thread, NULL, ThreadFunctionWrapper, thread);

    return thread;
}

// 将线程添加到线程池的空闲队列中
void AddThreadToIdleQueue(ThreadPool *pool, Thread *thread) {
    pthread_mutex_lock(&pool->lock);
    list_add_tail(&thread->node, &pool->idle_queue);
    pool->idlecount++;
    pthread_mutex_unlock(&pool->lock);
}

// 线程安全扩展线程池，不得在线程池锁作用域内使用
void ExpandThreadPool(ThreadPool *pool, void *(*pFunction)(void *), void *pArgs) {
    pthread_mutex_lock(&pool->lock);
    if((pool->activecount + pool->idlecount) == 0){
        // 线程池是空的先放最基本的线程吧
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    if(pool->idlecount > 0) {
        // 还有空闲线程就不扩了
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    int newThreadsCount = pool->activecount;  // 新增线程数等于当前工作线程数（加倍）
    for (int i = 0; i < newThreadsCount; i++) {
        list_add_tail(&(CreateThread(pool, pFunction, pArgs)->node), &pool->idle_queue);
        pool->idlecount++;
    }
    pthread_mutex_unlock(&pool->lock);
}

// 线程安全缩减线程池，不得在线程池锁作用域内使用
void ShrinkThreadPool(ThreadPool *pool, int minsize) {
    pthread_mutex_lock(&pool->lock);
    if(pool->idlecount <= minsize) {
        // 没必要缩减
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    int removeCount = pool->idlecount / 2;  // 减少线程数为当前空闲线程数一半
    if ( (pool->idlecount - removeCount) < minsize ) {
        // 保证无论如何空余线程数量都比设定的要高，至少是1
        removeCount = pool->idlecount - minsize;
    }
    while (removeCount > 0 && !list_empty(&pool->idle_queue)) {
        LIST_NODE *node = pool->idle_queue.next;
        Thread *thread = list_entry(node, Thread, node);
        list_del(node);
        pthread_cancel(thread->thread);  // 取消线程
        pthread_join(thread->thread, NULL);  // 等待线程结束
        free(thread);  // 释放线程资源
        pool->idlecount--;
        removeCount--;
    }
    pthread_mutex_unlock(&pool->lock);
}

// 销毁整个线程池
void destroyThreadPool(ThreadPool *pool) {
    pthread_mutex_lock(&pool->lock);
    // 取消所有线程
    LIST_NODE *pos, *tmp;
    list_for_each_safe(pos, tmp, &pool->work_queue) {
        Thread *thread = list_entry(pos, Thread, node);
        pthread_cancel(thread->thread);
        pthread_join(thread->thread, NULL);
        free(thread);
    }
    pool->activecount = 0;
    list_for_each_safe(pos, tmp, &pool->idle_queue) {
        Thread *thread = list_entry(pos, Thread, node);
        pthread_cancel(thread->thread);
        pthread_join(thread->thread, NULL);
        free(thread);
    }
    pool->idlecount = 0;
    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}
