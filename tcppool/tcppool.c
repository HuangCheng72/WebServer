//
// Created by huangcheng on 2024/5/5.
//

#include "tcppool.h"

// 活跃队列，就是有任务的
extern SocketTaskQueue* ActiveSocketQueue;
// 空闲队列，就是没任务的
extern SocketTaskQueue* IdleSocketQueue;

// 程序是否继续运行标志
extern int keep_running;

//交换两个元素，该函数不对外暴露
void exchange(TCPPOOL *pTCPPOOL, int i,int j){
    if(i == j){
        return;
    }
    //交换指针元素
    TCPINFO* temp = pTCPPOOL->pTCPINFO[i];
    pTCPPOOL->pTCPINFO[i] = pTCPPOOL->pTCPINFO[j];
    pTCPPOOL->pTCPINFO[j] = temp;
    //更新下标
    pTCPPOOL->pTCPINFO[i]->index = i;
    pTCPPOOL->pTCPINFO[j]->index = j;
}
//元素上浮，返回最终的位置，该函数不对外暴露
void swim(TCPPOOL *pTCPPOOL, int i){
    //如果下标为i处指针指向的TCPINFO剩余超时时间小于下标为i/2指向的TCPINFO剩余超时时间，就交换到前面去
    while(pTCPPOOL->pTCPINFO[i]->timeout < pTCPPOOL->pTCPINFO[i/2]->timeout){//因为下标为0处是哨兵，交换到下标为1就是堆顶了，不会再上浮
        exchange(pTCPPOOL, i, i/2);
        i /= 2;
    }
}
//元素下沉，该函数不对外暴露
void sink(TCPPOOL *pTCPPOOL, int i){
    //若是i处指针指向的TCPINFO剩余超时时间大于i*2或者i*2+1处指针指向的TCPINFO剩余超时时间，就交换到较小的那个位置
    //注意堆的规模是count
    while(2 * i <= pTCPPOOL->count){
        //比较i*2和i*2+1哪个更小
        int j = 2 * i;
        if(j < pTCPPOOL->count && pTCPPOOL->pTCPINFO[j+1]->timeout < pTCPPOOL->pTCPINFO[j]->timeout){
            j++;
        }
        if(pTCPPOOL->pTCPINFO[i]->timeout < pTCPPOOL->pTCPINFO[j]->timeout){
            //i处指针指向的TCPINFO剩余超时时间已经比两个当中最小的那一个都要小了，说明符合小顶堆性质，退出
            return;
        }
        //不符合小顶堆性质，就交换
        exchange(pTCPPOOL, i, j);
        //更新j的值
        i = j;
    }
}

TCPPOOL *CreateTCPPool() {
    int epoll_fd = epoll_create1(0);        // 创建epoll实例
    if (epoll_fd == -1) {
        // 创建失败就退出
        perror("epoll_create1 failed");
        return NULL;
    }

    TCPPOOL *pTCPPOOL = (TCPPOOL *)malloc(sizeof(TCPPOOL));
    if (!pTCPPOOL) {
        perror("Failed to allocate memory for TCPPOOL");
        close(epoll_fd);  // 确保即使失败也要关闭文件描述符
        return NULL;
    }

    pTCPPOOL->count = 0;
    pTCPPOOL->size = 128;    // 初始默认为128，后续支持扩容，所以要用这个记录容量

    pTCPPOOL->pTCPINFO = (TCPINFO **)malloc(sizeof(TCPINFO*) * pTCPPOOL->size); //动态数组申请方式
    if (!pTCPPOOL->pTCPINFO) {
        perror("Failed to allocate memory for TCPINFO");
        free(pTCPPOOL);
        close(epoll_fd);
        return NULL;
    }

    //初始化，指针数组置空
    for(int i = 0; i < pTCPPOOL->size; i++) {
        pTCPPOOL->pTCPINFO[i] = NULL;
    }

    //这个结点是同时作为头结点和哨兵结点用的
    TCPINFO *DummyNode = (TCPINFO *)malloc(sizeof(TCPINFO));
    DummyNode->client_socket = -1;      // 这表示它没有一个有效的TCP连接
    DummyNode->timeout = 0;             // 没有比它更小的值了，才能用作哨兵，挡住一切比它大的
    DummyNode->index = 0;               // 哨兵结点专属位置
    init_list_node(&(DummyNode->node));    // 初始化这个结点，让它可以发挥头结点的用途


    // 设置链表头结点，并且在指针数组里面也加入哨兵结点的位置
    pTCPPOOL->head = DummyNode;
    pTCPPOOL->pTCPINFO[0] = DummyNode;


    // 建立哈希表，大小为701是个质数，使用起来效果好一点
    pTCPPOOL->pHashTable_fd_pTCPINFO = CreateHashTableIntKey(701);

    // 连接池拥有的epoll实例
    pTCPPOOL->epoll_fd = epoll_fd;

    // 连接池的线程互斥锁
    pthread_mutex_init(&pTCPPOOL->mutex, NULL);

    // 返回设置好的头结点
    return pTCPPOOL;
}

// 扩容方法，不对外暴露，缩小容量暂时不考虑
void TCPPOOLResize(TCPPOOL *pTCPPOOL) {
    if(pTCPPOOL->pTCPINFO == NULL) {
        return;
    }
    //扩容策略就是最简单的两倍扩容
    int newsize = pTCPPOOL->size * 2;
    TCPINFO ** newPtrArray = (TCPINFO **)malloc(sizeof(TCPINFO*) * newsize);   //动态数组申请方式
    //初始化和拷贝信息
    for(int i = 0; i < newsize; i++) {
        if(i < pTCPPOOL->size) {
            newPtrArray[i] = pTCPPOOL->pTCPINFO[i];
        } else {
            newPtrArray[i] = NULL;
        }
    }
    //信息转移成功，销毁旧数组，设置新数组
    free(pTCPPOOL->pTCPINFO);
    pTCPPOOL->pTCPINFO = newPtrArray;
    pTCPPOOL->size = newsize;
}

TCPINFO *CreateTCPINFO(int client_socket, int timeout) {
    // 创建结构体
    TCPINFO *pTCPINFO = (TCPINFO *) malloc(sizeof(TCPINFO));
    // 管理信息
    pTCPINFO->index = -1;
    pTCPINFO->timeout = timeout;
    init_list_node(&(pTCPINFO->node));                     // 初始化结点

    // 处理信息
    pTCPINFO->client_socket = client_socket;

    return pTCPINFO;
}

void FreeTCPINFO(TCPINFO *pTCPINFO) {
    // 完全回收资源
    list_del(&(pTCPINFO->node));
    // 回收整个信息结构体
    free(pTCPINFO);
}

int AddTCPToTCPPool(TCPPOOL *pTCPPOOL, TCPINFO *pTCPINFO) {
    if (pTCPPOOL == NULL || pTCPPOOL->pHashTable_fd_pTCPINFO == NULL) {
        return 0;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pTCPPOOL->mutex)) {
        // 加锁失败，连接池出问题了
        return 0;
    }

    if (!HashTableIntKey_Contains(pTCPPOOL->pHashTable_fd_pTCPINFO, pTCPINFO->client_socket)) {
        // 将文件描述符添加到 epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;              // 监听读取事件，触发模式为边缘触发
        ev.data.fd = pTCPINFO->client_socket;       // 监听的文件描述符
        if (epoll_ctl(pTCPPOOL->epoll_fd, EPOLL_CTL_ADD, pTCPINFO->client_socket, &ev) == -1) {
            // 无法监听，添加失败
            perror("Failed to add socket to epoll");
            pthread_mutex_unlock(&pTCPPOOL->mutex);   // 解锁
            return 0;
        } else {
            // 设置管理信息
            pTCPINFO->index = ++pTCPPOOL->count;                // 管理TCP连接的数量增多，每次都先插入到堆底位置

            // 只需要设置管理信息就行了，处理信息是不变的

            // 放置并管理
            if (pTCPPOOL->count == pTCPPOOL->size) {
                // 满了，扩容
                TCPPOOLResize(pTCPPOOL);
            }

            pTCPPOOL->pTCPINFO[pTCPPOOL->count] = pTCPINFO;     // 插入到堆底位置
            list_add(&(pTCPPOOL->head->node), &(pTCPINFO->node));     // 存入存储结构
            swim(pTCPPOOL, pTCPPOOL->count);                    // 堆底元素上浮，完成堆的性质维护

            HashTableIntKey_Insert(pTCPPOOL->pHashTable_fd_pTCPINFO, pTCPINFO->client_socket, pTCPINFO);  // 建立文件描述符和TCP连接信息结构体指针之间的关系

            pthread_mutex_unlock(&pTCPPOOL->mutex);   // 解锁
            return 1;
        }
    }
}

int RemoveTCPFromTCPPool(TCPPOOL *pTCPPOOL, TCPINFO *pTCPINFO) {
    // 首先，先判断这一元素是不是这个连接池管理的
    // 如果连接池当前管理的堆规模是0，说明这个不属于这个连接池管理
    // 如果TCP结点存储的下标比连接池的总规模还大，说明也不属于这个连接池管理
    // 后面三个条件说明已经被移除了
    if(pTCPPOOL->count == 0 || pTCPPOOL->size <= pTCPINFO->index || pTCPINFO->timeout == -1 || pTCPINFO->index == -1 || pTCPINFO->client_socket == -1){
        return -1;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pTCPPOOL->mutex)) {
        // 加锁失败，连接池出问题了
        return -1;
    }

    // 而后从epoll队列中移除，归还系统资源是最优先的操作
    if (epoll_ctl(pTCPPOOL->epoll_fd, EPOLL_CTL_DEL, pTCPINFO->client_socket, NULL) == -1) {
        // 无法从epoll队列移除，终止在连接池中的其他操作
        perror("Failed to remove socket from epoll");
        pthread_mutex_unlock(&pTCPPOOL->mutex);   // 解锁
        return -1;
    } else {
        // 首先把这个指针交换到堆末尾，然后再缩小堆的规模
        // 缩小堆的规模，就意味着不再受到堆的管理，不需要真的执行删除操作
        int index = pTCPINFO->index;
        exchange(pTCPPOOL, index, pTCPPOOL->count);
        --pTCPPOOL->count;
        sink(pTCPPOOL, index);              // 然后调整顺序（因为是从堆底交换过来的，所以要下沉），完成堆的性质维护

        list_del(&(pTCPINFO->node));           // 从存储链表中删除这个TCP连接
        HashTableIntKey_Delete(pTCPPOOL->pHashTable_fd_pTCPINFO, pTCPINFO->client_socket);  // 删除文件描述符和TCP连接信息结构体指针之间的关系

        // 把这个连接信息从连接池中取出，还是要处理的，因此要保留处理信息，管理信息可以删掉，防止出错
        pTCPINFO->timeout = -1;             // 设置为一个不可能的值，防止出错
        pTCPINFO->index = -1;               // 设置为一个不可能的值，防止出错
        int fd = pTCPINFO->client_socket;
        pTCPINFO->client_socket = -1;       // 设置为一个不可能的值，防止出错
        init_list_node(&(pTCPINFO->node));   // 让这个结点成为一个自环，也就是初始化状态

        pthread_mutex_unlock(&pTCPPOOL->mutex);   // 解锁
        return fd;
    }
}

void FreeTCPPool(TCPPOOL *pTCPPOOL) {
    if (!pTCPPOOL) return;

    // 直接关闭所有的TCP连接，然后从连接池中删除所有连接
    for (int i = 1; i < pTCPPOOL->count; i++) {
        TCPINFO *pTCPINFO = pTCPPOOL->pTCPINFO[i];
        // 关闭连接并回收信息结构体
        if (pTCPINFO && pTCPINFO->client_socket > -1) {
            close(pTCPINFO->client_socket);
            pTCPINFO->client_socket = -1;   //防止再被错误关闭
        }
        // 回收信息结构体
        RemoveTCPFromTCPPool(pTCPPOOL, pTCPINFO);
        FreeTCPINFO(pTCPINFO);
    }
    // 回收互斥锁
    pthread_mutex_destroy(&pTCPPOOL->mutex);
    // 回收指针数组和哈希表
    FreeHashTableIntKey(pTCPPOOL->pHashTable_fd_pTCPINFO);
    free(pTCPPOOL->pTCPINFO);
    // 回收头结点
    free(pTCPPOOL->head);
    // 回收整个连接池
    free(pTCPPOOL);
}

void PrintTCPPoolStatus(TCPPOOL *pTCPPOOL) {
    if(!pTCPPOOL) return;
    pthread_mutex_lock(&pTCPPOOL->mutex);
    printf("TCP连接池中现有连接数量为: %d\n", pTCPPOOL->count);
    pthread_mutex_unlock(&pTCPPOOL->mutex);
}

// 连接池管理线程函数
void* tcppool_thread(void* arg) {
    TCPPOOL * pTCPPOOL = (TCPPOOL *)arg;

    // 注册当前线程到计时管理线程
    TimerThreadControl *timer_ctrl = register_timer_thread(pthread_self());
    // 存在依赖关系，不得不重试
    while (!timer_ctrl) {
        timer_ctrl = register_timer_thread(pthread_self());
        if (!timer_ctrl) {
            sleep(1);  // 睡眠1秒后重试
        }
    }

    int events_size = pTCPPOOL->size / 10;
    // 事件数组大小设置为十分之一的管理规模
    struct epoll_event *events = (struct epoll_event *)malloc(events_size * sizeof(struct epoll_event));

    //每个被添加进来的一定是加入epoll了，不要重复添加了

    while (1 && keep_running) {

        // 等待计时器线程的信号，阻塞时间极短，可以认为是非阻塞方式
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // 获取当前时间

        pthread_mutex_lock(timer_ctrl->mutex);
        // 设置超时时间为当前时间，即不等待
        int wait_result = pthread_cond_timedwait(timer_ctrl->cond, timer_ctrl->mutex, &now);
        pthread_mutex_unlock(timer_ctrl->mutex);

        if(wait_result == 0) {
            // 收到信号，已经过了一秒钟了
            // 堆顶是下标为1的位置
            for(int i = 1; i < pTCPPOOL->count + 1; i++) {
                --pTCPPOOL->pTCPINFO[i]->timeout;
            }
            // 检查有没有剩余超时时间已经为0的（因为是最小堆，如果堆顶都没超时，其他的更不用说），有的话就全部关掉
            while(pTCPPOOL->count > 0 && pTCPPOOL->pTCPINFO[1]->timeout <= 0) {
                // 从连接池中删除并关闭其连接
                TCPINFO *pTCPINFOtop = pTCPPOOL->pTCPINFO[1];
                int fd = RemoveTCPFromTCPPool(pTCPPOOL, pTCPINFOtop);
                FreeTCPINFO(pTCPINFOtop);      // 回收连接信息
                if(fd > -1) {
                    close(fd);
                }
            }
        }

        // TCP连接池扩容过了，那就跟着扩容
        if (pTCPPOOL->size / 10 > events_size) {
            struct epoll_event *new_events = (struct epoll_event *)malloc((pTCPPOOL->size / 10) * sizeof(struct epoll_event));
            for(int i = 0; i < events_size; i++) {
                new_events[i] = events[i];
            }
            events = new_events;
            events_size = pTCPPOOL->size / 10;
        }

        // 检查空闲队列里面有没有需要入池的连接
        while(!IsSocketQueueEmpty(IdleSocketQueue)) {

            int fd = GetFromSocketQueue(IdleSocketQueue);
            // 检查是否有数据
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            // 设置超时为0，使select立即返回
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;

            int ret = select(fd + 1, &readfds, NULL, NULL, &timeout);
            if (ret > 0) {
                // 有数据，出门左转活跃队列等待处理
                AddToSocketQueue(ActiveSocketQueue, fd);
            } else if (ret == 0) {
                // 没有数据，入池管理，默认超时时间60秒
                AddTCPToTCPPool(pTCPPOOL, CreateTCPINFO(fd, 60));
            } else {
                // 没办法了只能关掉链接
                close(fd);
            }
        }

        // 使用 epoll_wait 函数等待事件发生
        // epoll_fd：epoll 实例的文件描述符
        // events：用于存储发生的事件的数组
        // events_size：events 数组的大小
        // timeout：指定超时时间为-1，除非有事件发生，否则一直阻塞，如果为0则是立即返回的非阻塞状态，如果是正数则是阻塞时长
        // 返回结果 nfds 表示的发生事件的数量
        int nfds = epoll_wait(pTCPPOOL->epoll_fd, events, events_size, 0);

        if (nfds == -1) {
            // 检查 epoll_wait 是否失败
            perror("epoll_wait failed");
            continue;  // 如果失败，继续循环（在实际代码中可以考虑不同的处理方式）
        }

        for (int n = 0; n < nfds; ++n) {
            // 判断当前事件是否是可读事件（EPOLLIN 表示数据可读）
            if (events[n].events & EPOLLIN) {
                // 获取发生事件的文件描述符（即有数据可读的 socket）
                int fd = events[n].data.fd;
                // 找到对应的指向 TCPINFO 的指针
                TCPINFO *pTCPINFO = HashTableIntKey_Find(pTCPPOOL->pHashTable_fd_pTCPINFO, fd);
                if (pTCPINFO != NULL) {
                    if(RemoveTCPFromTCPPool(pTCPPOOL, pTCPINFO) != -1) {
                        // 有任务，当然要放入任务就绪队列中等待处理
                        AddToSocketQueue(ActiveSocketQueue, fd);
                    }
                }
            }
        }
    }
    //释放整个TCP管理体系
    FreeTCPPool(pTCPPOOL);
    pthread_exit(NULL);
}