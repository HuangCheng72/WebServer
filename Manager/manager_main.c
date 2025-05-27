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
 * 进程池和工作进程设计思路：
 * 1. 工作进程功能最简单化：处理HTTP请求和上报心跳包
 * 2. 工作进程的状态由进程池管理和实时更新
 */

// 防止多个进程同时持有同一个socket，导致Listener无法关闭TCP连接的情况
// fork+exec方式会保留文件描述符给启动的新进程，管理进程就是通过fork+exec方式启动工作进程
// 给工作进程发待处理的socket之前，必须要全部关闭管理进程持有的socket，只留下管理进程与工作进程通信的socket
// 工作进程处理完毕之后立即close，管理进程发回Listener的时候也会关闭管理进程持有的这一份，使得最终只有Listener持有
// Listener负责最终的关闭，完成TCP连接的关闭

// 为了方便fork+exec启动工作进程之前，在子进程环境下关闭管理进程持有的所有socket，增加一个链表变式
// 唯一目的就是记录管理进程持有的所有TCP连接

// 该数据结构唯一的Add操作发生在RecvFromListenerThread线程中
// 唯一的Remove操作发生在SendToListenerThread线程中
// 所以倒是不需要加锁保护，因为本来就没有线程冲突

// 因此在资源元素里面加上第二个链表元素，直接记录管理进程持有的所有TCP连接

// 记录所有TCP连接的总链表的头结点
LIST_NODE all_socket_list_head;

// 对于总链表而言唯一的作用就是用于fork+exec时关闭管理进程所持有的TCP连接
// 因此针对这个场景直接封装函数close_all_socket

// socket信息结构体
typedef struct {
    int client_socket;          // 代表用户的TCP连接，在Linux中是一个文件描述符
    LIST_NODE node;             // 链表结点
    LIST_NODE all_node;         // 记录所有TCP连接的链表结点
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

    // 直接在创建时加入到总链表里面，这样就不用管了
    init_list_node(&(temp->all_node));
    list_add_tail(&temp->all_node, &all_socket_list_head);

    return temp;
}

/**
 * 销毁socket信息
 * @param pInfo socket信息结构体指针
 */
void Destroy_socketinfo(socketinfo *pInfo) {
    list_del(&pInfo->node);     // 删除结点，防止影响到其前后的结点

    list_del(&pInfo->all_node); // 从总链表中删除

    if(pInfo->client_socket > -1) {
        close(pInfo->client_socket);
    }
    memset(pInfo, 0, sizeof(socketinfo));
    free(pInfo);
}

/**
 * 关闭总链上管理的所有TCP连接
 */
void close_all_socket() {
    struct list_node *pos = NULL;
    list_for_each(pos, &all_socket_list_head) {
        close(list_entry(pos, socketinfo, all_node)->client_socket);
    }
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
    // 尝试加锁，如果锁不在自己手里，不得销毁
    if (pthread_mutex_trylock(&queue->lock)) {
        // 加锁失败，正在被占用
        return;
    }
    struct list_node *pos = NULL;
    list_for_each(pos, &queue->head) {
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
    // 尝试加锁
    if (pthread_mutex_trylock(&queue->lock)) {
        // 加锁失败，正在被占用，操作失败
        return 0;
    }
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
    // 尝试加锁
    if (pthread_mutex_trylock(&queue->lock)) {
        // 加锁失败，正在被占用，操作失败
        return NULL;
    }
    socketinfo *temp = list_entry(queue->head.next, socketinfo, node);
    list_del(&temp->node);     // 删除结点，防止影响到其前后的结点
    queue->size--;
    pthread_mutex_unlock(&queue->lock);
    return temp;
}

// Listener发来的等待处理的连接，进入等待队列
socketinfo_queue *wait_queue = NULL;
// 处理完毕的连接，等待发回Listener
socketinfo_queue *done_queue = NULL;

// 与守护进程通信的 socket（用于接收与管理进程交互的两个socket，以及上报心跳包）
int Daemon_Main_Socket = -1;

// 这是与监听进程交互的两个Socket
int send_to_Listener_Process_Socket = -1;
int recv_from_Listener_Process_Socket = -1;

// 程序运行标志
int keep_running;
// 秒标志，每隔一秒变动一次
volatile int second_flag;

// done队列的数据，从send_to_Listener_Process_Socket发出去
void *SendToListenerThread(void *arg) {
    while (keep_running) {
        socketinfo *info = Remove_socketinfo_from_queue(done_queue);
        if (info) {
            if (fd_send(send_to_Listener_Process_Socket, info->client_socket) < 0) {
                perror("[ERROR] Failed to send fd to Listener");
            }
            Destroy_socketinfo(info);
        } else {
            sched_yield();  // 队列为空时让出CPU
        }
    }

    // 解释说明：
    // socket发到管理进程后，在监听进程已经被close，等管理进程分配给工作进程处理完毕之后再发回监听进程
    // 监听进程才持有另一份，而发回监听进程之后，在管理进程这里也是被close
    // 因此，在管理进程退出时关闭，socket也是可以被彻底关闭的（此时管理进程持有的是最后一份）

    pthread_exit(NULL);
}

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
    // socket发到管理进程后，在监听进程已经被close，等管理进程分配给工作进程处理完毕之后再发回监听进程
    // 监听进程才持有另一份，而发回监听进程之后，在管理进程这里也是被close
    // 因此，在管理进程退出时关闭，socket也是可以被彻底关闭的（此时管理进程持有的是最后一份）

    pthread_exit(NULL);
}

/**
 * 工作进程本身并不上报状态，它只要还活着就足够了（它只要还每秒上报心跳包就行）
 *
 * 工作进程的任务：
 * 1. 接收待处理的TCP连接，分发给协程处理（一个连接由一个协程负责）。
 * 2. 处理完毕就发回给管理进程，这本质上也是告诉管理进程，已经处理完毕，而且还可以在工作进程中滞留一段时间，应对可能的连续请求情况
 * 3. 每秒上报一个心跳包，表示自己还活着（不需要告诉管理进程自己当前有多少请求等候处理，因为没必要，管理进程这边都记录状态）
 *
 * 状态由管理进程中对应负责这个工作进程的线程（叫监视线程并不恰当，因为任务不只是监视，应该叫负责线程）全面管理
 * 工作进程也不能靠自己去抢占，只能由进程池分配任务
 *
 * 负责线程的任务：
 * 1. 维护这个工作进程对应的信息结构体
 * 2. 如果有新任务，就发给工作进程
 * 3. 如果工作进程超时很久没反应，就杀掉（不需要重启，但是需要把没完成的任务交还给进程池管理线程）
 * 4. 如果进程池管理线程要求负责线程关闭工作进程退出就退出，如果不要求就不退出
 *
 * 进程池管理线程的任务：
 * 1. 维护负责线程的状态，是否要求其退出等
 * 2. 把wait队列中的任务分发给负责线程发出去
 * 3. 确定已经完成的任务，加入到done队列中，等着发回Listener
 *
 * Manager这里负责工作进程的全部工作如下：
 * 1. 心跳接收（负责接收所有工作进程的心跳包，并且更新心跳时间）
 * 2. 任务分配（负责把全局wait队列中的任务分发到各个工作进程信息的wait队列中）
 * 3. 发出自身的wait队列中待处理的TCP连接给对应的工作进程
 * 4. 接收处理完毕的TCP连接，回收到自身的done队列中
 * 5. 任务回收（负责把各个工作进程信息中的已完成任务回收到全局done队列）
 * 6. 进程池容量调节（如果任务过多，需要创建更多的工作进程，只能一个个创建，如果任务过少，就一个个减少没用的）
 * 7. 杀掉某个特定的工作进程（不考虑优雅退出，直接暴力杀掉，确定对方确实已经退出之后把这个工作进程的TCP连接回收到全局队列，等待再次分配）
 *
 * 角色分配：
 * 1. 负责线程负责各自工作进程的心跳接收、杀死工作进程、发送和接收TCP连接（就是1、2、3和4、7）
 * 2. 进程池管理线程负责任务分配、任务回收、容量调节（2、5、6）
 *
 * 注意事项：
 * 1. 工作进程的负责线程必须要等所负责的工作进程退出之后，自己才能退出
 * 2. 工作进程退出之后，才能把工作进程信息里面的TCP连接取走重新分配
 * 3. 杀死工作进程后，由负责线程直接把持有的wait和done交还给全局队列（用flags让进程池管理线程跳过）
 * 4. 如果要退出，退出顺序是，工作进程退出，负责工作进程的负责线程才能退出，所有的负责线程全部退出，则进程池管理线程可以退出，所有线程退出后管理进程才能退出
 *
 */

// 对应的数据结构

// 工作进程信息
typedef struct {
    pid_t pid;                  // 工作进程的pid
    pthread_t monitor;          // 该工作线程的负责线程

    int workload;               // 工作进程的负载量（只有等待处理的，即workload = wait->size，处理完毕的就不需要理会了）

    int flags;                  // 初始设置为0，如果为1，就是要求杀掉这个工作进程，如果为-1，就是这个工作进程已经被杀死了

    socketinfo_queue *wait;     // 等待交给工作进程处理的TCP连接
    socketinfo_queue *done;     // 已经处理完成，等待移交给全局done队列的TCP连接

    struct timespec last_work_time; // 最近一次工作时间

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

    temp->wait = Create_socketinfo_queue();
    temp->done = Create_socketinfo_queue();

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
    Destroy_socketinfo_queue(pInfo->done);

    list_del(&pInfo->node);

    memset(pInfo, 0, sizeof(WorkerInfo));

    free(pInfo);
}

// 工作进程的负责线程
void *Workers_Monitor_Thread(void *arg) {
    if(!arg) {
        return NULL;
    }
    WorkerInfo *pInfo = (WorkerInfo *)arg;

    struct timespec last_heartbeat_time = {0};

    const int max_timeout_ms = 5000;            // 心跳超时时间（超过这个时间没有心跳包出来，就认为所负责的工作进程已经死了，强制杀死

    SOCKETPAIR heart_beat_manager_worker;       // 用于worker发心跳包给manager（也是最开始的传输通道）
    socketpair_create(&heart_beat_manager_worker);

    SOCKETPAIR manager_send_worker_recv;        // 用于worker接收等待处理的TCP连接
    SOCKETPAIR manager_recv_worker_send;        // 用于worker发回已经完成工作的TCP连接
    socketpair_create(&manager_send_worker_recv);
    socketpair_create(&manager_recv_worker_send);

    StatusMessage tmp;

    pid_t pid;

    // 启动工作进程

    // fork前准备工作，把传输TCP的双向专用通道发过去（顺序是对于对方而言先发送，后接收，看的是右边是发送还是接收）
    fd_send(heart_beat_manager_worker.sock1, manager_recv_worker_send.sock2);
    fd_send(heart_beat_manager_worker.sock1, manager_send_worker_recv.sock2);

    pid = fork();
    if (pid < 0) {
        perror("[ERROR] Failed to fork");
        return NULL;
    }

    if (pid == 0) {
        // pid == 0时，此时是在子进程中了
        // fork+exec 方式启动进程，可以保留文件描述符资源

        // 关闭所有不属于这个工作进程的文件描述符资源

        // 关闭管理进程当前持有的所有socket（再传过去的就只是给它的工作任务了）
        close_all_socket();

        // 关掉管理进程持有的其他socket
        close(send_to_Listener_Process_Socket);
        close(recv_from_Listener_Process_Socket);
        close(Daemon_Main_Socket);

        // 关掉属于管理进程的一端
        close(heart_beat_manager_worker.sock1);

        // 把这两个全部关掉，工作进程只允许获得它能获得的
        socketpair_destroy(&manager_send_worker_recv);
        socketpair_destroy(&manager_recv_worker_send);

        // 其他资源比如全局变量等，在exec之后会被替换，无需清理

        char fd_str[32];
        snprintf(fd_str, sizeof(fd_str), "%d", heart_beat_manager_worker.sock2);  // 将文件描述符转为字符串

        // 用execl来启动功能进程
        execl("./Worker", "WebServer_Worker", fd_str, NULL);
        // 如果 execl 返回，表示执行失败
        perror("[ERROR] Failed to exec Worker");

        exit(1);    // 这里退出的是子进程

    } else {
        // 父进程：启动完成，记录信息
        pInfo->pid = pid;
        printf("[INFO] Worker process started with PID: %d\n", pid);
    }

    // 停顿5秒，让系统完成进程启动工作
    sleep(5);
    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);

    while (keep_running) {

        // 如果外力要求杀死工作进程，那就立即执行
        if(pInfo->flags == 1) {
            // 立即退出主循环
            break;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(heart_beat_manager_worker.sock1, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(heart_beat_manager_worker.sock1 + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            // 这表明出问题了，必须立即退出主循环
            pInfo->flags = 1;   // 正在杀死工作进程
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳过本轮
            goto check_timeout_and_restart;
        }

        // 有数据可读
        int ret = recv_status_message(heart_beat_manager_worker.sock1, &tmp);
        if (ret == 0) {
            clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
        }

check_timeout_and_restart:
        // 心跳超时判断
        if (pid > 0) {
            long delta_ms =
                    (now.tv_sec - last_heartbeat_time.tv_sec) * 1000 +
                    (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000000;

            if (delta_ms > max_timeout_ms) {
                // 进程已死，打出正在杀死标志
                pInfo->flags = 1;
                // 立即退出主循环
                break;
            }
        }

        // 如果等待队列有socketinfo，就发给工作进程
        if (!IsEmpty_socketinfo_queue(pInfo->wait)) {
            // 发过去之后，管理进程这边持有的TCP连接的文件描述符关闭，只剩下工作进程持有这一TCP连接
            // 如果客户要求不用keep-alive，那就不会再发回来了（工作进程关闭最后的这一份，TCP连接关闭）
            // 如果客户要求keep-alive，处理完成就还会发回来

            while(!IsEmpty_socketinfo_queue(pInfo->wait)) {
                socketinfo *info = Remove_socketinfo_from_queue(pInfo->wait);
                if (info) {
                    if (fd_send(manager_send_worker_recv.sock1, info->client_socket) < 0) {
                        perror("[ERROR] Failed to send fd to Worker");
                        // 这表明出问题了，必须立即杀死工作进程
                        pInfo->flags = 1;   // 正在杀死工作进程
                        goto kill_worker;
                    }
                    Destroy_socketinfo(info);
                    // 更新工作负载量
                    pInfo->workload = pInfo->wait->size;
                    // 更新最近一次工作时间
                    clock_gettime(CLOCK_MONOTONIC, &pInfo->last_work_time);
                } else {
                    // 队列为空则退出循环
                    break;
                }
            }
        }

        // 检查是否有已处理完成的socket发来，循环读到没有为止（不知道发来的有多少个）（注意，select会改变结果，每次select之后都要重设fd_set）
        fd_set readfds_socket;

        while(1) {

            FD_ZERO(&readfds_socket);
            FD_SET(manager_recv_worker_send.sock1, &readfds_socket);

            sel_ret = select(manager_recv_worker_send.sock1 + 1, &readfds_socket, NULL, NULL, &timeout);
            if (sel_ret < 0) {
                perror("[ERROR] select failed");
                // 这表明出问题了，必须立即杀死工作进程
                pInfo->flags = 1;   // 正在杀死工作进程
                goto kill_worker;
            } else if (sel_ret == 0) {
                // 没有数据就退出不读
                break;
            }  else {
                // 有数据可读，则循环读到没有数据为止
                int client_socket = fd_recv(manager_recv_worker_send.sock1);
                if(client_socket < 0) {
                    // 这表明出问题了，必须立即杀死工作进程
                    pInfo->flags = 1;   // 正在杀死工作进程
                    goto kill_worker;
                }
                // 把接收到的socket加到完成队列中，等待发回Listener
                Add_socketinfo_to_queue(pInfo->done, Create_socketinfo(client_socket));

                // 更新工作负载量
                pInfo->workload = pInfo->wait->size;
                // 更新最近一次工作时间
                clock_gettime(CLOCK_MONOTONIC, &pInfo->last_work_time);
            }
        }

        usleep(200 * 1000);  // 200ms 轮询间隔
    }

kill_worker:
    // 无论出于什么原因，退出线程主循环，离开线程之前，必须杀死所负责的工作进程

    // 强制杀掉Worker
    printf("[INFO] The worker process (PID: %d) will be terminated.\n", pid);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    // 更改为已经杀死标志
    pInfo->flags = -1;
    // pid置为-1防止误操作
    pInfo->pid = -1;

    // 清理资源
    socketpair_destroy(&manager_send_worker_recv);
    socketpair_destroy(&manager_recv_worker_send);
    socketpair_destroy(&heart_beat_manager_worker);

    // 将所有的任务归还给全局队列，等待重新分配给其他工作进程
    while(!IsEmpty_socketinfo_queue(pInfo->wait)) {
        Add_socketinfo_to_queue(wait_queue, Remove_socketinfo_from_queue(pInfo->wait));
    }
    while(!IsEmpty_socketinfo_queue(pInfo->done)) {
        Add_socketinfo_to_queue(done_queue, Remove_socketinfo_from_queue(pInfo->done));
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

    struct list_node *pos = NULL;
    list_for_each(pos, &pool->head) {
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

    // 根据给定参数创建初始的工作进程
    for(int i = 0; i < pool->min_count; i++) {
        WorkerInfo *pInfo = Create_WorkerInfo();
        Add_WorkInfo_to_WorkerPool(pool, pInfo);
        // 启动工作进程的负责线程，由它完成后续具体工作
        pthread_create(&pInfo->monitor, NULL, Workers_Monitor_Thread, pInfo);
    }

    // 以 wait_queue->size 为标准判断是否需要增加一个工作进程
    int current_workload = wait_queue->size;

    // 增加一个工作进程的条件是：所有工作进程都已满，而且current_workload至少是max_workload的一半，则允许增加一个工作进程，但是不能超过最大工作进程数量
    // 减少一个工作进程的条件是：首先要大于min_count，而且三十秒的时间里，这个工作进程没事干，workload一直都是0，就可以关掉这个工作进程，一次只能关一个

    struct list_node *pos = NULL;

    while (keep_running) {

        // 一直更新当前负载量
        current_workload = wait_queue->size;

        // 删除所有已经死亡的工作进程信息
        list_for_each(pos, &pool->head) {
            WorkerInfo *worker_info = list_entry(pos, WorkerInfo, node);
            if(worker_info->flags == -1) {
                // flags的值，0是正常运行状态，1是需要被杀死，-1是已经被杀死
                // 这是已经死亡的工作进程信息，将其移出进程池并且销毁
                Remove_WorkInfo_from_WorkerPool(pool, worker_info);
                Destroy_WorkerInfo(worker_info);
            }
        }

        // 判断是否需要增加工作进程
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
                pool->count++;

            }
        }

        // 判断是否需要减少工作进程
        list_for_each(pos, &pool->head) {
            WorkerInfo *worker_info = list_entry(pos, WorkerInfo, node);

            // 如果工作进程的负载一直为0，且已超过30秒，则考虑杀掉工作进程
            if (worker_info->workload == 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);

                long delta_ms =
                        (now.tv_sec - worker_info->last_work_time.tv_sec) * 1000 +
                        (now.tv_nsec - worker_info->last_work_time.tv_nsec) / 1000000;

                if (delta_ms >= 30000 && pool->count > pool->min_count) {
                    if(worker_info->flags == 0) {
                        // 还在正常工作的情况下三十秒之内没任务，才需要去杀死它，否则不需要去杀它

                        worker_info->flags = 1;  // 设置标记要求杀死工作进程
                        // 等待线程退出
                        pthread_join(worker_info->monitor, NULL);

                        break; // 一次只能关掉一个工作进程
                    }
                }
            }
        }

        // 任务分发
        while (!IsEmpty_socketinfo_queue(wait_queue)) {
            socketinfo *task = Remove_socketinfo_from_queue(wait_queue);
            if (task) {
                // 找到一个空闲工作进程
                WorkerInfo *worker_info = NULL;

                list_for_each(pos, &pool->head) {
                    worker_info = list_entry(pos, WorkerInfo, node);
                    // 工作进程必须是正常运行状态才能由进程池管理线程分发任务
                    if ((worker_info->flags == 0) && (worker_info->workload < pool->max_workload)) {
                        // 将任务分配给该工作进程
                        Add_socketinfo_to_queue(worker_info->wait, task);
                        // 更新该工作进程当前负载量
                        worker_info->workload = worker_info->wait->size;
                        break;
                    }
                }
            }
        }

        // 任务回收
        list_for_each(pos, &pool->head) {
            WorkerInfo *worker_info = list_entry(pos, WorkerInfo, node);
            // 处理完成的任务，从 done 队列转移到全局队列
            // 工作进程必须是正常运行状态才能由进程池管理线程回收任务
            while ((worker_info->flags == 0) && !IsEmpty_socketinfo_queue(worker_info->done)) {
                socketinfo *task = Remove_socketinfo_from_queue(worker_info->done);
                Add_socketinfo_to_queue(done_queue, task);
            }
        }

        usleep(200 * 1000);  // 200ms 轮询间隔
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
    send_to_Listener_Process_Socket = fd_recv(Daemon_Main_Socket);
    recv_from_Listener_Process_Socket = fd_recv(Daemon_Main_Socket);
    if (send_to_Listener_Process_Socket < 0 || recv_from_Listener_Process_Socket < 0) {
        fprintf(stderr, "Failed to connect to Listener_Process.\n");
        return -1;
    }

    keep_running = 1;
    second_flag = 0;

    // 初始化总链表头结点
    init_list_node(&all_socket_list_head);

    // 创建工作队列
    wait_queue = Create_socketinfo_queue();
    done_queue = Create_socketinfo_queue();

    // 启动两个线程
    pthread_t recv_thread, send_thread;
    pthread_create(&recv_thread, NULL, RecvFromListenerThread, NULL);
    pthread_create(&send_thread, NULL, SendToListenerThread, NULL);

    // 启动进程池管理线程
    pthread_t manage_thread;

    WorkerPool *pool = Create_WorkerPool();
    pool->min_count = 8;    // 初始工作进程数量设置为8
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
            printf("[INFO] Listener received shutdown signal.\n");
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
    pthread_join(manage_thread, NULL);
    pthread_join(recv_thread, NULL);
    pthread_join(send_thread, NULL);

    // 销毁资源
    Destroy_socketinfo_queue(wait_queue);
    Destroy_socketinfo_queue(done_queue);

    // 清理自己持有的所有socket
    close(send_to_Listener_Process_Socket);
    close(recv_from_Listener_Process_Socket);
    close(Daemon_Main_Socket);

    printf("[INFO] WebServer_Manager exit.\n");

    return 0;
}
