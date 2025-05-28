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
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <errno.h>

// 客户信息结构体
typedef struct {
    int client_socket;          // 代表用户的TCP连接，在Linux中是一个文件描述符
    int keep_alive;             // keep-alive标志
    LIST_NODE node;             // 链表结点
} clientinfo;

/**
 * 创建客户信息
 * @param client_socket socket的文件描述符
 * @return 客户信息结构体指针
 */
clientinfo *Create_clientinfo(int client_socket) {
    clientinfo *temp = (clientinfo*)malloc(sizeof(clientinfo));
    if(!temp) {
        return NULL;
    }
    init_list_node(&(temp->node));
    temp->client_socket = client_socket;
    temp->keep_alive = 1;   // 默认都是keep-alive

    return temp;
}

/**
 * 销毁客户信息
 * @param pInfo 客户信息结构体指针
 */
void Destroy_clientinfo(clientinfo *pInfo) {
    // 这里被销毁之前已经删除过了，再次删除就会出现NULL->prev和NULL->next的情况
    if (!pInfo){
        return;
    }

    if(pInfo->client_socket > -1) {
        close(pInfo->client_socket);
    }
    memset(pInfo, 0, sizeof(clientinfo));
    free(pInfo);
}

/**
 * 客户信息队列结构体
 */
typedef struct {
    LIST_NODE head;
    int size;
    pthread_mutex_t lock;
} clientinfo_queue;

/**
 * 创建客户信息队列
 * @return 客户信息队列结构体指针
 */
clientinfo_queue *Create_clientinfo_queue() {
    clientinfo_queue *temp = (clientinfo_queue*)malloc(sizeof(clientinfo_queue));
    if(!temp) {
        return NULL;
    }
    init_list_node(&(temp->head));
    temp->size = 0;
    pthread_mutex_init(&temp->lock, NULL);  // 整个队列的互斥锁
    return temp;
}

/**
 * 销毁客户信息队列
 * 因为使用场景确定为最终资源清理，因此修改为关闭队列中所有的socket文件描述符
 * @param queue
 */
void Destroy_clientinfo_queue(clientinfo_queue *queue) {
    if(!queue || list_empty(&queue->head)) {
        return;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    // 涉及增删链表结点的操作，必须使用 list_for_each_safe
    struct list_node *pos = NULL;
    struct list_node *n = NULL;
    list_for_each_safe(pos, n, &queue->head) {
        list_del(pos);
        Destroy_clientinfo(list_entry(pos, clientinfo, node));
        queue->size--;
    }

    init_list_node(&queue->head);
    // 锁在自己手里，解锁，销毁。
    pthread_mutex_unlock(&queue->lock);
    free(queue);
}

/**
 *  客户信息队列是否为空
 * @param queue 要判断的客户信息队列
 * @return 为空返回1，不为空返回0
 */
int IsEmpty_clientinfo_queue(clientinfo_queue *queue) {
    if(!queue) {
        return 0;   // 无法判断
    }
    return list_empty(&queue->head) && queue->size == 0;
}

/**
 * 把一个客户信息添加到queue
 * @param queue queue的指针
 * @param pInfo 要添加的客户信息
 * @return 成功返回1，失败返回0
 */
int Add_clientinfo_to_queue(clientinfo_queue *queue, clientinfo *pInfo) {
    if(!queue || !pInfo) {
        return 0;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);
    // 加锁成功可以进行
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
clientinfo *Remove_clientinfo_from_queue(clientinfo_queue *queue) {
    // 获取第一个元素
    if(list_empty(&queue->head)) {
        return NULL;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    clientinfo *temp = list_entry(queue->head.next, clientinfo, node);
    list_del(&temp->node);     // 删除结点，防止影响到其前后的结点
    queue->size--;
    pthread_mutex_unlock(&queue->lock);
    return temp;
}

// 等待处理的连接
clientinfo_queue *waiting_handle = NULL;
// 处理完毕等着入池的连接
clientinfo_queue *done_queue = NULL;

// 与管理进程通信的 socket（用于接收与管理进程交互的两个socket，以及上报心跳包）
int Manager_Socket = -1;

// 发处理完成的TCP连接给监听进程重新入池
int send_to_Listener_Process_Socket_keep_alive = -1;
// 发处理完成的TCP连接给监听进程关掉
int send_to_Listener_Process_Socket_close = -1;

// 从管理进程接收需要处理的TCP连接
int recv_from_Manager_Process_Socket = -1;

// 程序运行标志
int keep_running;

// 托管目录
#define WEB_ROOT "/home/hc/web"

void handle_http_request() {

    // 直接从队列里面取
    clientinfo *pInfo = Remove_clientinfo_from_queue(waiting_handle);
    if (!pInfo) {
        // 队列为空或者出错，直接退出
        return;
    }
    int client_socket = pInfo->client_socket;
    if(client_socket == -1) {
        // 连接出错退出
        return;
    }

    char buffer[4096];

    while (1) {
        // 确定是否有数据到达，有就处理
        int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
        if(bytes_read > 0) {
            buffer[bytes_read] = '\0';
            char method[10], path[1024], protocol[10];
            if (sscanf(buffer, "%s %s %s", method, path, protocol) != 3) {
                continue; // 如果没有解析到完整的请求行，继续监听
            }

            //这个变量是用于确定还要不要继续连接下去
            int keep_alive = 1; //HTTP/1.1默认就是持久连接

            if(strcasecmp("HTTP/1.0", protocol) == 0) {
                keep_alive = 0; //HTTP/1.0默认没有持久连接
            }

            char *connection_header = strstr(buffer, "Connection:");

            if (connection_header && strstr(connection_header, "keep-alive")) {
                keep_alive = 1; //再次明确到底有没有持久连接
            }
            
            // 记录到这个连接信息里面
            pInfo->keep_alive = keep_alive;

            // 处理完毕之后，这里不能close，必须放到队列中，由队列统一close

            // 响应头
            char response[4096];

            if (strcasecmp(method, "GET") == 0) {
                // 拼接完整路径
                char full_path[1024];
                sprintf(full_path, "%s%s", WEB_ROOT, strcmp(path, "/") == 0 ? "/index.html" : path);

                // 同样是打开文件，这是Linux的系统调用，权限是只读
                int file = open(full_path, O_RDONLY);
                if (file) {
                    // stat是Linux内核里面声明的一个结构体，Linux的文件的所有信息就存储在这样的一个结构体里面
                    struct stat stat_buf;
                    // 用系统调用，把文件的信息保存到我们刚刚创造的结构体临时变量里面
                    fstat(file, &stat_buf);
                    //格式输出到响应头
                    //stat_buf.st_size就是文件的大小属性，也就是我们要发送的内容长度
                    //升级到HTTP/1.1协议了
                    sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: %s\r\n\r\n", stat_buf.st_size, keep_alive ? "keep-alive" : "close");   //客户要求持久连接，服务器就采取持久连接，否则就不持久连接。
                    if (write(client_socket, response, strlen(response)) == -1) {
                        perror("response_head");
                        // 出错，绝对不能keep-alive，必须关掉
                        pInfo->keep_alive = 0;
                        break;
                    }
                    //零拷贝直接发送文件
                    sendfile(client_socket, file, NULL, stat_buf.st_size);
                    //发送完毕，关闭文件
                    close(file);
                } else {
                    char *error_message = "404 Not Found";
                    sprintf(response, "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s", strlen(error_message), keep_alive ? "keep-alive" : "close", error_message);
                    if (write(client_socket, response, strlen(response)) == -1) {
                        perror("response_head");
                        // 出错，绝对不能keep-alive，必须关掉
                        pInfo->keep_alive = 0;
                        break;
                    }
                }
            } else {
                char *error_message = "501 Not Implemented";
                sprintf(response, "HTTP/1.1 501 Not Implemented\r\nConnection: %s\r\n\r\n%s", keep_alive ? "keep-alive" : "close", error_message);
                if (write(client_socket, response, strlen(response)) == -1) {
                    perror("response_head");
                    // 出错，绝对不能keep-alive，必须关掉
                    pInfo->keep_alive = 0;
                    break;
                }
            }
        } else if (bytes_read == 0) {
            // 读取到EOF，客户端关闭连接，那服务器这边也要关闭最后一份连接

            // alive置为0
            pInfo->keep_alive = 0;
            break;

        } else {
            // 如果没有读取到数据，并且不是因为阻塞，说明出错
            if (errno != EAGAIN || errno != EWOULDBLOCK) {
                perror("read error");
                // alive置为0
                pInfo->keep_alive = 0;
                break;
            } else {
                // 阻塞说明没数据，处理完毕

                // 没数据的情况说明处理完了，退出就是了
                break;
            }
        }
    }
    // 操作完成，关闭写流（不然没法close）
    shutdown(pInfo->client_socket, SHUT_WR);
    // 统一操作，把完成操作的客户信息装入队列，判断是否要发回去
    Add_clientinfo_to_queue(done_queue, pInfo);
}

// done队列的数据，从 send_to_Listener_Process_Socket 发出去
void *SendToListenerThread(void *arg) {
    while (keep_running) {
        clientinfo *info = Remove_clientinfo_from_queue(done_queue);
        if (info) {
            if (info->keep_alive) {
                if (fd_send(send_to_Listener_Process_Socket_keep_alive, info->client_socket) < 0) {
                    perror("[ERROR] Failed to send fd to Manager (Worker1)");
                }
            } else {
                if (fd_send(send_to_Listener_Process_Socket_close, info->client_socket) < 0) {
                    perror("[ERROR] Failed to send fd to Manager (Worker2)");
                }
            }
            // 处理完成的就全部关闭了
            Destroy_clientinfo(info);
        } else {
            sched_yield();  // 队列为空时让出CPU
        }
    }

    pthread_exit(NULL);
}

// 从 recv_from_Manager_Process_Socket 接收的fd，放入wait队列
void *RecvFromManagerThread(void *arg) {
    struct timeval tv;
    fd_set read_fds;

    while (keep_running) {
        FD_ZERO(&read_fds);
        FD_SET(recv_from_Manager_Process_Socket, &read_fds);

        tv.tv_sec = 0;
        tv.tv_usec = 0; // 非阻塞检查

        int ret = select(recv_from_Manager_Process_Socket + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(recv_from_Manager_Process_Socket, &read_fds)) {
            int client_fd = fd_recv(recv_from_Manager_Process_Socket);
            if (client_fd >= 0) {
                // 在监听进程中已经设置为非阻塞模式了
                clientinfo *info = Create_clientinfo(client_fd);
                if (info) {
                    Add_clientinfo_to_queue(waiting_handle, info);
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

// 工作线程
void *WorkThread(void *arg) {

    while (keep_running) {
        handle_http_request();
    }

    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    // 参数校验
    if (argc < 4) {
        fprintf(stderr, "[ERROR] No file descriptor passed as argument\n");
        exit(1);
    }

    Manager_Socket = atoi(argv[1]);                                 // 将传递的文件描述符转换为整数
    send_to_Listener_Process_Socket_keep_alive = atoi(argv[2]);     // 将传递的文件描述符转换为整数
    send_to_Listener_Process_Socket_close = atoi(argv[3]);          // 将传递的文件描述符转换为整数

    // 使用传递过来的文件描述符，进行后续的监听或通信
    printf("[INFO] Worker Received Manager_Socket: %d\n", Manager_Socket);

    recv_from_Manager_Process_Socket = fd_recv(Manager_Socket);
    if (recv_from_Manager_Process_Socket < 0) {
        fprintf(stderr, "Worker failed to connect to Manager_Process.\n");
        return -1;
    }

    keep_running = 1;

    // 创建资源队列
    waiting_handle = Create_clientinfo_queue();
    done_queue = Create_clientinfo_queue();

    // 启动两个线程
    pthread_t send_thread, recv_thread, work_thread;
    pthread_create(&send_thread, NULL, SendToListenerThread, NULL);
    pthread_create(&recv_thread, NULL, RecvFromManagerThread, NULL);
    pthread_create(&work_thread, NULL, WorkThread, NULL);

    printf("[INFO] WebServer_Worker started.\n");

    struct timespec ts_target, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_target);

    // 这里的status只上报pid这个有用的信息，其他情况下只作为心跳包使用
    StatusMessage status;
    status.module = 3;  // 代表worker
    status.pid = getpid();


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

        // 发送心跳包
        if (send_status_message(Manager_Socket, &status) < 0) {
            perror("Failed to send status to Manager");
        }
    }

    keep_running = 0;

    // 等待线程全部退出
    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);
    pthread_join(work_thread, NULL);

    // 销毁资源
    Destroy_clientinfo_queue(waiting_handle);
    Destroy_clientinfo_queue(done_queue);

    // 清理自己持有的所有socket
    close(send_to_Listener_Process_Socket_keep_alive);
    close(send_to_Listener_Process_Socket_close);
    close(recv_from_Manager_Process_Socket);
    close(Manager_Socket);

    printf("[INFO] WebServer_Worker exit.\n");

    return 0;
}
