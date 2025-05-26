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
    list_del(&pInfo->node);     // 删除结点，防止影响到其前后的结点
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
socketinfo_queue *Create_tcpinfo_queue() {
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
int IsEmpty_socketinfo_queuee(socketinfo_queue *queue) {
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
    list_del(&pInfo->node);
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

    pthread_exit(NULL);
}


// 与守护进程通信的 socket（用于接收与监听进程交互的两个socket，以及上报心跳包）
int Daemon_Main_Socket = -1;

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

    // 创建工作队列
    wait_queue = Create_tcpinfo_queue();
    done_queue = Create_tcpinfo_queue();

    // 启动两个线程
    pthread_t recv_thread, send_thread;
    pthread_create(&recv_thread, NULL, RecvFromListenerThread, NULL);
    pthread_create(&send_thread, NULL, SendToListenerThread, NULL);

    struct timespec ts_target, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_target);

    // 管理进程要上报给守护进程的信息
    StatusMessage status;
    // 约定使用参数顺序如下
    // StatusMessage.status_args[] 中顺序如下：
    // [0] = 模块号（固定：1 = Listener）
    // [1] = PID

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
        status.module = 2; // 监听模块编号
        status.pid = getpid();

        if (send_status_message(Daemon_Main_Socket, &status) < 0) {
            perror("Failed to send status to Daemon");
        }
    }

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
