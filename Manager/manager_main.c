//
// Created by huangcheng on 2025/5/21.
//

#include "list.h"
#include "transfer_fd.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>

/**
 * 设计变更总原则：TCP连接流动的方向改为单向流动，方向为：Listener -> Manager -> Worker -> Listener
 */

/**
 * 进程池和工作进程设计思路：
 * 1. 工作进程功能最简单化：处理HTTP请求和上报心跳包（管理进程把任务TCP连接发给工作进程，工作进程处理完毕发回Listener，工作进程给管理进程上报心跳包）
 * 2. 工作进程的状态由进程池管理和实时更新
 * 3. 每个工作进程由一个负责进程负责（作用是监控这个工作进程的心跳和发送分配这个工作进程的任务）
 * 4. 进程池的作用：分配任务、任务量过大就要求建立工作进程来处理、任务量不够，就杀死多余的工作进程（一个个增，一个个减）
 *
 */

// socket信息结构体
typedef struct {
    int client_socket;          // 代表用户的TCP连接，在Linux中是一个文件描述符
    LIST_NODE node;             // 链表结点
} socketinfo;

/**
 * 创建socket信息
 * @param client_socket socket的文件描述符
 * @return socket信息结构体指针
 */
socketinfo *Create_socketinfo(int client_socket) {
    socketinfo *temp = (socketinfo*)malloc(sizeof(socketinfo));
    if(!temp) {
        return NULL;
    }
    init_list_node(&(temp->node));
    temp->client_socket = client_socket;

    return temp;
}

/**
 * 销毁socket信息
 * @param pInfo socket信息结构体指针
 */
void Destroy_socketinfo(socketinfo *pInfo) {
    // 这里被销毁之前已经删除过了，再次删除就会出现NULL->prev和NULL->next的情况
    if (!pInfo){
        return;
    }

    if(pInfo->client_socket > -1) {
        close(pInfo->client_socket);
    }
    memset(pInfo, 0, sizeof(socketinfo));
    free(pInfo);
}

/**
 * socket信息队列结构体
 */
typedef struct {
    LIST_NODE head;
    int size;
    pthread_mutex_t lock;
} socketinfo_queue;

/**
 * 创建socket信息队列
 * @return socket信息队列结构体指针
 */
socketinfo_queue *Create_socketinfo_queue() {
    socketinfo_queue *temp = (socketinfo_queue*)malloc(sizeof(socketinfo_queue));
    if(!temp) {
        return NULL;
    }
    init_list_node(&(temp->head));
    temp->size = 0;
    pthread_mutex_init(&temp->lock, NULL);  // 整个队列的互斥锁
    return temp;
}

/**
 * 销毁socket信息队列
 * 因为使用场景确定为最终资源清理，因此修改为关闭队列中所有的socket文件描述符
 * @param queue
 */
void Destroy_socketinfo_queue(socketinfo_queue *queue) {
    if(!queue || list_empty(&queue->head)) {
        return;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    // 涉及增删链表结点的操作，必须使用list_for_each_safe
    struct list_node *pos = NULL;
    struct list_node *n = NULL;
    list_for_each_safe(pos, n, &queue->head) {
        list_del(pos);
        Destroy_socketinfo(list_entry(pos, socketinfo , node));
        queue->size--;
    }
    init_list_node(&queue->head);
    // 锁在自己手里，解锁，销毁。
    pthread_mutex_unlock(&queue->lock);
    free(queue);
}

/**
 *  socket信息队列是否为空
 * @param queue 要判断的socket信息队列
 * @return 为空返回1，不为空返回0
 */
int IsEmpty_socketinfo_queue(socketinfo_queue *queue) {
    if(!queue) {
        return 0;   // 无法判断
    }
    return list_empty(&queue->head) && queue->size == 0;
}

/**
 * 把一个socket信息添加到queue
 * @param queue queue的指针
 * @param pInfo 要添加的socket信息
 * @return 成功返回1，失败返回0
 */
int Add_socketinfo_to_queue(socketinfo_queue *queue, socketinfo *pInfo) {
    if(!queue || !pInfo) {
        return 0;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    // 加锁成功可以进行
    // list_del(&pInfo->node);  // 如果已经删除了，那么再次删除就会出现NULL->prev和NULL->next的问题，所以，这里不该删除，应该在remove里面删除
    list_add_tail(&pInfo->node, &queue->head);
    queue->size++;
    pthread_mutex_unlock(&queue->lock);
    return 1;
}

/**
 * 从queue中取出队头元素
 * @param queue queue的指针
 * @return 队头元素的指针
 */
socketinfo *Remove_socketinfo_from_queue(socketinfo_queue *queue) {
    // 获取第一个元素
    if(list_empty(&queue->head)) {
        return NULL;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    socketinfo *temp = list_entry(queue->head.next, socketinfo, node);
    list_del(&temp->node);     // 删除结点，防止影响到其前后的结点
    queue->size--;
    pthread_mutex_unlock(&queue->lock);
    return temp;
}

// Listener发来的等待处理的连接，进入等待队列
socketinfo_queue *wait_queue = NULL;

// 与守护进程通信的 socket（用于接收两个socket，以及上报心跳包）
int Daemon_Main_Socket = -1;

// 这是与监听进程交互的Socket
int recv_from_Listener_Process_Socket = -1;
// 这是要求守护进程创建Worker的专用通道
int create_worker_socket;

// 程序运行标志
int keep_running;
// 秒标志，每隔一秒变动一次
volatile int second_flag;

// 从recv_from_Listener_Process_Socket接收的fd，放入wait队列
void *RecvFromListenerThread(void *arg) {
    struct timeval tv;
    fd_set read_fds;

    while (keep_running) {
        FD_ZERO(&read_fds);
        FD_SET(recv_from_Listener_Process_Socket, &read_fds);

        tv.tv_sec = 0;
        tv.tv_usec = 0; // 非阻塞检查

        int ret = select(recv_from_Listener_Process_Socket + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(recv_from_Listener_Process_Socket, &read_fds)) {
            int client_fd = fd_recv(recv_from_Listener_Process_Socket);
            if (client_fd >= 0) {
                // 在监听进程中已经设置为非阻塞模式了
                socketinfo *info = Create_socketinfo(client_fd);
                if (info) {
                    Add_socketinfo_to_queue(wait_queue, info);
                } else {
                    close(client_fd);
                }
            }
        }

        if (ret <= 0) {
            sched_yield();  // 没有事件，让出CPU
        }
    }

    // 解释说明：
    // 管理进程只起到转发作用，发出之后就Destroy_socketinfo，自然也就close掉这个TCP连接，不持有这一份
    // TCP的最终关闭都是在监听进程进行的

    pthread_exit(NULL);
}

// 对应的数据结构

// 工作进程信息
typedef struct {
    pid_t pid;                  // 工作进程的pid
    pthread_t monitor;          // 该工作线程的负责线程

    int workload;               // 工作进程的负载量（只有等待处理的，即workload = wait->size，处理完毕的就不需要理会了）

    int flags;                  // 初始设置为0，如果为1，就是要求杀掉这个工作进程，如果为-1，就是这个工作进程已经被杀死了

    socketinfo_queue *wait;     // 等待交给工作进程处理的TCP连接

    // 解释，管理进程始终持有这两条通道，所以就算Worker崩了，也影响不到管理进程
    // 因为在管理进程中这两对socketpair一直有效，内核不会真正关闭，不会出现一端关闭导致另一端崩掉的情况

    SOCKETPAIR manager_worker;      // Manager和Worker之间最早的联系通道，负责初期传输第二条联系通道，以及后续心跳进程

    SOCKETPAIR manager_send_worker_recv;    // 第二条联系通道，专用于发任务TCP连接给工作进程

    LIST_NODE node;             // 工作进程不可能就一个，需要动态扩容或者缩小，所以用链表存储

} WorkerInfo;

/**
 * 创建一个工作进程信息结构体
 * @return 返回一个工作进程信息结构体指针
 */
WorkerInfo *Create_WorkerInfo() {
    WorkerInfo *temp = (WorkerInfo *)malloc(sizeof(WorkerInfo));
    if(!temp) {
        return NULL;
    }
    memset(temp, 0, sizeof(WorkerInfo));
    temp->pid = -1;     // 这是为了防止判断出错

    socketpair_create(&temp->manager_worker);
    socketpair_create(&temp->manager_send_worker_recv);

    temp->wait = Create_socketinfo_queue();

    init_list_node(&temp->node);

    return temp;
}

/**
 * 销毁一个工作进程信息
 * @param pInfo 工作进程信息结构体指针
 */
void Destroy_WorkerInfo(WorkerInfo *pInfo) {
    if(!pInfo) {
        return;
    }
    Destroy_socketinfo_queue(pInfo->wait);

    list_del(&pInfo->node);

    socketpair_destroy(&pInfo->manager_worker);
    socketpair_destroy(&pInfo->manager_send_worker_recv);

    memset(pInfo, 0, sizeof(WorkerInfo));

    free(pInfo);
}

// 工作进程的负责线程
void *Workers_Monitor_Thread(void *arg) {
    if(!arg) {
        return NULL;
    }
    WorkerInfo *pInfo = (WorkerInfo *)arg;

    pInfo->pid = -2;        // 开始启动进程的时候，防止被误杀
    pInfo->flags = 2;       // 开始启动进程的时候，防止被误杀

    struct timespec last_heartbeat_time = {0};
    struct timespec now = {0};

    const int max_timeout_ms = 5000;            // 心跳超时时间（超过这个时间没有心跳包出来，就认为所负责的工作进程已经死了，强制杀死

    // 创建Worker之前准备工作，先把准备的第二条联系通道发过去
    fd_send(pInfo->manager_worker.sock1, pInfo->manager_send_worker_recv.sock2);    // 按照规则，worker在右边，把sock2分配给worker

    StatusMessage tmp;
    // 要求Daemon创建Worker
    tmp.module = 6;
    tmp.args[0] = 7;

    // 约定的两个数据
    send_status_message(create_worker_socket, &tmp);
    fd_send(create_worker_socket, pInfo->manager_worker.sock2); // 按照规则，worker在右边，把sock2分配给worker

    // 等待五秒钟，如果五秒钟之内，worker都没把第二条联系通道收走，那就说明创建失败

    int success_flag = 0;   // 启动worker成功标志

    // 等待10秒
    int wait_time_ms = 10000;
    for (int i = 0; i < wait_time_ms / 100; i++) {

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pInfo->manager_worker.sock2, &readfds);

        // 轮询监控分配给worker的manager_worker.sock2，如果被收走了，就说明成功了

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(pInfo->manager_worker.sock2 + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            // 这表明出问题了，必须立即退出线程
            pInfo->flags = -1;   // 没成功启动，直接杀死

            return NULL;
        } else if (sel_ret == 0) {
            // 没有数据，说明启动成功了
            success_flag = 1;
            break;
        }

        // 有数据可读就等着，等到没数据可读为止

        usleep(100 * 1000);
    }

    if(success_flag == 0) {
        // 还是没成功，那就退出吧
        perror("[ERROR] cannot create worker");

        return NULL;
    }

    // 启动成功进行接下来的工作

    // 为了方便理解，接下来重命名几个变量

    int heart_beat_socket = pInfo->manager_worker.sock1;  // 此时最初联系通道已经完成了传输第二条传输通道的使命，完全用于接收心跳
    int send_to_worker_socket = pInfo->manager_send_worker_recv.sock1;  // 传输任务的专用通道

    long delta_ms = 0;  // 用于计算心跳间隔判断是否超时

    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);

    while (keep_running) {

        // 如果外力要求杀死工作进程，那就立即执行
        if(pInfo->flags == 1) {
            // 立即退出主循环
            break;
        }

        // 监控心跳和超时问题

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(heart_beat_socket, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(heart_beat_socket + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            // 这表明出问题了，必须立即退出主循环
            pInfo->flags = 1;   // 正在杀死工作进程
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳过本轮
            goto check_timeout;
        }

        // 有数据可读
        int ret = recv_status_message(heart_beat_socket, &tmp);
        if (ret == 0) {
            // 记录worker上报的pid
            if(pInfo->pid == -2) {
                printf("[INFO] Worker process started with PID: %d\n", tmp.pid);
            }
            pInfo->pid = tmp.pid;
            pInfo->flags = 0;   // 正常工作中
            clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
        }

check_timeout:

        clock_gettime(CLOCK_MONOTONIC, &now);
        // 心跳超时判断
        delta_ms = (now.tv_sec - last_heartbeat_time.tv_sec) * 1000 + (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000000;

        if (delta_ms > max_timeout_ms) {
            // 进程已死，打出正在杀死标志
            pInfo->flags = 1;
            // 立即退出主循环
            break;
        }

        // 如果等待队列有socketinfo，就发给工作进程
        while(!IsEmpty_socketinfo_queue(pInfo->wait) && (pInfo->flags == 0)) {
            // 只要不为空，Remove_socketinfo_from_queue出来的肯定有效
            socketinfo *info = Remove_socketinfo_from_queue(pInfo->wait);
            if (fd_send(send_to_worker_socket, info->client_socket) < 0) {
                perror("[ERROR] Failed to send fd to Worker");
                // 这表明出问题了，必须立即杀死工作进程
                pInfo->flags = 1;   // 正在杀死工作进程
                goto kill_worker;
            }
            Destroy_socketinfo(info);
            // 更新工作负载量
            pInfo->workload = pInfo->wait->size;
        }

        usleep(200 * 1000);  // 200ms 轮询间隔
    }

kill_worker:
    // 无论出于什么原因，退出线程主循环，离开线程之前，必须杀死所负责的工作进程

    if (pInfo->pid > -1) {
        // 强制杀掉Worker
        printf("[INFO] The worker process (PID: %d) will be terminated.\n", pInfo->pid);
        kill(pInfo->pid, SIGKILL);
        waitpid(pInfo->pid, NULL, 0);
    }

    // 更改为已经杀死标志
    pInfo->flags = -1;
    // pid置为-1防止误操作
    pInfo->pid = -1;

    // 将所有的任务归还给全局队列，等待重新分配给其他工作进程
    while(!IsEmpty_socketinfo_queue(pInfo->wait)) {
        Add_socketinfo_to_queue(wait_queue, Remove_socketinfo_from_queue(pInfo->wait));
    }

    // 清零当前的工作负载
    pInfo->workload = 0;

    pthread_exit(NULL);
}

// 工作进程池结构体
typedef struct {
    int count;      // 当前进程池中的工作进程数
    int min_count;  // 最少要有多少个工作进程存在，不然就没有工作进程了
    int size;       // 进程池容量，也就是数量上限（防止没完没了创建大量进程）

    int max_workload;   // 每个工作进程最大的负载量，如果工作进程的负载量达到这个水平，就不再给它分配任务

    LIST_NODE head; // 工作进程信息链表头

} WorkerPool;

/**
 * 创建一个工作进程池结构体
 * @return 返回一个工作进程池结构体指针
 */
WorkerPool *Create_WorkerPool() {
    WorkerPool *temp = (WorkerPool *)malloc(sizeof(WorkerPool));
    if(!temp) {
        return NULL;
    }
    memset(temp, 0, sizeof(WorkerPool));

    init_list_node(&temp->head);

    return temp;
}

/**
 * 销毁一个工作进程池
 * @param pool 工作进程池结构体指针
 */
void Destroy_WorkerPool(WorkerPool *pool) {
    if(!pool) {
        return;
    }
    // 有多少全部回收干净

    // 涉及增删链表结点的操作，必须使用 list_for_each_safe
    struct list_node *pos = NULL;
    struct list_node *n = NULL;
    list_for_each_safe(pos, n,&pool->head) {
        list_del(pos);
        Destroy_WorkerInfo(list_entry(pos, WorkerInfo, node));
    }

    init_list_node(&pool->head);

    memset(pool, 0, sizeof(WorkerPool));

    free(pool);
}

/**
 * 把一个工作进程信息加入进程池
 * @param pool 工作进程池结构体指针
 * @param pInfo 工作进程信息结构体指针
 */
void Add_WorkInfo_to_WorkerPool(WorkerPool *pool, WorkerInfo *pInfo) {
    if(!pool || !pInfo) {
        return;
    }
    list_add_tail(&pInfo->node, &pool->head);
    pool->count++;
}

/**
 * 把一个工作进程信息移出进程池
 * @param pool 工作进程池结构体指针
 * @param pInfo 工作进程信息结构体指针
 */
void Remove_WorkInfo_from_WorkerPool(WorkerPool *pool, WorkerInfo *pInfo) {
    if(!pool || !pInfo) {
        return;
    }
    list_del(&pInfo->node);
    pool->count--;
}

/**
 * 进程池是否为空
 * @param pool 工作进程池结构体指针
 * @return 为空则为1，不为空则为0
 */
int IsEmpty_WorkerPool(WorkerPool *pool) {
    if(!pool) {
        return 0;
    }
    return list_empty(&pool->head);
}

/**
 * 获取队头的工作进程信息
 * @param pool 工作进程池结构体指针
 * @return 工作进程信息结构体指针
 */
WorkerInfo *Get_First_WorkInfo_from_WorkerPool(WorkerPool *pool) {
    if(!pool || list_empty(&pool->head)) {
        return NULL;
    }
    struct list_node *temp = pool->head.next;
    WorkerInfo *pInfo = list_entry(temp, WorkerInfo, node);
    return pInfo;
}

void *WorkerPool_Manager_Thread(void *arg) {
    WorkerPool *pool = (WorkerPool *)arg;

    // 以 wait_queue->size 为标准判断是否需要增加一个工作进程
    int current_workload = wait_queue->size;

    // 增加一个工作进程的条件是：所有工作进程都已满，而且current_workload至少是max_workload的一半，则允许增加一个工作进程，但是不能超过最大工作进程数量
    // 减少一个工作进程的条件是：首先要大于min_count，而且三十秒的时间里，这个工作进程没事干，workload一直都是0，就可以关掉这个工作进程，一次只能关一个

    struct list_node *pos = NULL;
    struct list_node *n = NULL;

    while (keep_running) {

        // 一直更新当前负载量
        current_workload = wait_queue->size;

        // 删除所有已经死亡的工作进程信息

        // 涉及增删链表结点的操作，必须使用 list_for_each_safe
        list_for_each_safe(pos, n, &pool->head) {
            WorkerInfo *worker_info = list_entry(pos, WorkerInfo, node);
            if(worker_info->flags == -1) {
                // flags的值，0是正常运行状态，1是需要被杀死，-1是已经被杀死
                // 这是已经死亡的工作进程信息，将其移出进程池并且销毁
                Remove_WorkInfo_from_WorkerPool(pool, worker_info);
                Destroy_WorkerInfo(worker_info);
            }
        }

        // 判断是否需要增加工作进程（两种情况，一种情况是工作量太大，另一种情况是进程意外崩掉导致进程池中存留线程数小于最小线程数）

        // 意外崩掉（这也是初始创建逻辑）
        if(pool->count < pool->min_count) {
            WorkerInfo *new_worker = Create_WorkerInfo();
            Add_WorkInfo_to_WorkerPool(pool, new_worker);
            // 启动负责线程
            pthread_create(&new_worker->monitor, NULL, Workers_Monitor_Thread, new_worker);
        }


        // 工作量太大
        if (current_workload >= (pool->max_workload / 2)) {
            // 检查所有工作进程是否都已满
            int all_full = 1;
            list_for_each(pos, &pool->head) {
                WorkerInfo *worker_info = list_entry(pos, WorkerInfo, node);
                if (worker_info->workload < pool->max_workload) {
                    all_full = 0;
                    break;
                }
            }

            // 如果所有工作进程都已满载，且当前待处理任务数达到条件，则增加一个工作进程
            if (all_full && (pool->count < pool->size)) {
                WorkerInfo *new_worker = Create_WorkerInfo();
                Add_WorkInfo_to_WorkerPool(pool, new_worker);
                // 启动负责线程
                pthread_create(&new_worker->monitor, NULL, Workers_Monitor_Thread, new_worker);

            }
        }

        // 判断是否需要减少工作进程（暂时取消）

        // 任务分发
        while (!IsEmpty_socketinfo_queue(wait_queue)) {
            // 只要队列不为空，Remove_socketinfo_from_queue出来的肯定有效
            socketinfo *task = Remove_socketinfo_from_queue(wait_queue);
            // 找到一个空闲工作进程
            WorkerInfo *worker_info = NULL;

            list_for_each(pos, &pool->head) {
                worker_info = list_entry(pos, WorkerInfo, node);
                // 工作进程必须是正常运行状态才能由进程池管理线程分发任务
                if ((worker_info->flags == 0) && (worker_info->workload < pool->max_workload)) {
                    // 找到合适的工作进程就退出
                    break;
                }
                worker_info = NULL;
            }

            if(worker_info) {
                // 将任务分配给该工作进程
                Add_socketinfo_to_queue(worker_info->wait, task);
                // 更新该工作进程当前负载量
                worker_info->workload = worker_info->wait->size;
            } else {
                // 找不出来就加回去
                Add_socketinfo_to_queue(wait_queue, task);
            }
        }

        // 任务回收（任务不回收了）

        usleep(100 * 1000);  // 100ms 轮询间隔
    }

    // 等待所有的工作进程的负责线程全部退出，进程池管理线程销毁进程池，即可退出
    while(!IsEmpty_WorkerPool(pool)) {
        WorkerInfo *pInfo = Get_First_WorkInfo_from_WorkerPool(pool);
        // 要求监视线程停止还在工作的工作进程（为1的时候正在杀死工作进程，为-1的时候已经杀死工作进程）
        if (pInfo->flags == 0) {
            pInfo->flags = 1;
            // 等待线程退出
            pthread_join(pInfo->monitor, NULL);
        }
        // 删掉这个工作进程结构体
        Remove_WorkInfo_from_WorkerPool(pool, pInfo);
        Destroy_WorkerInfo(pInfo);
    }

    Destroy_WorkerPool(pool);

    pthread_exit(NULL);
}


int main(int argc, char *argv[]) {
    // 参数校验
    if (argc < 2) {
        fprintf(stderr, "[ERROR] No file descriptor passed as argument\n");
        exit(1);
    }

    Daemon_Main_Socket = atoi(argv[1]);  // 将传递的文件描述符转换为整数

    // 使用传递过来的文件描述符，进行后续的监听或通信
    printf("[INFO] Manager Received Daemon_Main_Socket: %d\n", Daemon_Main_Socket);

    // 接收与 Listener 的通信 socket
    recv_from_Listener_Process_Socket = fd_recv(Daemon_Main_Socket);
    if (recv_from_Listener_Process_Socket < 0) {
        fprintf(stderr, "Failed to connect to Listener_Process.\n");
        return -1;
    }
    create_worker_socket = fd_recv(Daemon_Main_Socket);
    if (create_worker_socket < 0) {
        fprintf(stderr, "Failed to create worker.\n");
        return -1;
    }

    keep_running = 1;
    second_flag = 0;

    // 创建工作队列
    wait_queue = Create_socketinfo_queue();

    // 启动两个线程
    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, RecvFromListenerThread, NULL);

    // 启动进程池管理线程
    pthread_t manage_thread;

    WorkerPool *pool = Create_WorkerPool();
    pool->min_count = 1;    // 初始工作进程数量设置为8
    pool->size = 128;       // 进程池中工作进程上限设置为128（容量设置为128）
    pool->max_workload = 1024;  // 每个工作进程的最大负载量为1024

    pthread_create(&manage_thread, NULL, WorkerPool_Manager_Thread, pool);

    struct timespec ts_target, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_target);

    // 管理进程要上报给守护进程的信息
    StatusMessage status;
    // 约定使用参数顺序如下
    // StatusMessage.status_args[] 中顺序如下：
    // [0] = 模块号（固定：2 = Manager）
    // [1] = PID
    // [2] = 进程池容量
    // [3] = 每个工作进程的最大负载量
    // [4] = 当前进程池中的工作进程数量
    // [5] = wait_queue中还有多少连接待分配给具体的工作进程处理

    // 守护进程发来的控制信息
    StatusMessage control_msg;

    printf("[INFO] WebServer_Manager started.\n");

    while (keep_running) {
        // 目标时间：每次加1秒
        ts_target.tv_sec += 1;

        // 等待直到目标时间
        while (keep_running) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);

            // 如果已达到或超过目标时间，跳出
            if ((ts_now.tv_sec > ts_target.tv_sec) ||
                (ts_now.tv_sec == ts_target.tv_sec && ts_now.tv_nsec >= ts_target.tv_nsec)) {
                break;
            }

            // 否则主动让出CPU
            sched_yield();
        }

        // 更新计时标志
        second_flag ^= 1;  // 翻转标志（0和1异或得1，1和1异或得0）

        // === 使用 select 等待可读 ===
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(Daemon_Main_Socket, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(Daemon_Main_Socket + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳到发送心跳包
            goto heartbeat_report;
        }

        int ret = recv_status_message(Daemon_Main_Socket, &control_msg);

        /**
         * shutdown_msg的魔数为：
         * args[0] = -25;
         * args[3] = -41;
         * args[10] = -99;
         */

        if (ret == 0 && control_msg.args[0] == -25 && control_msg.args[3] == -41 && control_msg.args[10] == -99) {
            printf("[INFO] Manager received shutdown signal.\n");
            keep_running = 0;
            break;
        }

heartbeat_report:
        // 发送信息（也起到心跳包的作用）
        memset(&status, 0, sizeof(status));
        status.module = 2; // 管理模块编号
        status.pid = getpid();
        status.args[0] = pool->size;
        status.args[1] = pool->max_workload;
        status.args[2] = pool->count;
        status.args[3] = wait_queue->size;

        if (send_status_message(Daemon_Main_Socket, &status) < 0) {
            perror("Failed to send status to Daemon");
        }
    }

    // 等待线程全部退出
    pthread_join(recv_thread, NULL);
    pthread_join(manage_thread, NULL);

    // 销毁资源
    Destroy_socketinfo_queue(wait_queue);

    // 清理自己持有的所有socket
    close(create_worker_socket);
    close(recv_from_Listener_Process_Socket);
    close(Daemon_Main_Socket);

    printf("[INFO] WebServer_Manager exit.\n");

    return 0;
}
