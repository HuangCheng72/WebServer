#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include "timer/timerthread.h"
#include "list/socketqueue.h"
#include "tcppool/tcppool.h"
#include "threadpool/threadpool.h"
#include "threadpool/threadpoolmanager.h"
#include "socketserver/socketserver.h"


// 这里我需要解释一下，"/home/hc/web"是我的网站文件夹目录，可以随便修改，或者改成用参数接受不同的路径也可以。
#define WEB_ROOT "/home/hc/web"

// 程序退出标志
volatile int keep_running = 1;

// 活跃队列，就是有任务的
SocketTaskQueue* ActiveSocketQueue = NULL;
// 空闲队列，就是没任务的
SocketTaskQueue* IdleSocketQueue = NULL;

// HTTP请求处理函数
void handle_http_request();

// 用于判断是否有任务需要处理的函数
int function() { return !IsSocketQueueEmpty(ActiveSocketQueue); }

int main() {
    // 初始化全局队列
    ActiveSocketQueue = CreatedSocketTaskQueue();
    IdleSocketQueue = CreatedSocketTaskQueue();

    // 开启计时线程
    pthread_t timer_thread_id;
    pthread_create(&timer_thread_id, NULL, timer_thread, NULL);
    pthread_detach(timer_thread_id);

    // 开启连接池管理线程
    pthread_t tcppool_thread_id;
    TCPPOOL *pTCPPOOL = CreateTCPPoll();
    pthread_create(&tcppool_thread_id, NULL, tcppool_thread, pTCPPOOL);
    pthread_detach(tcppool_thread_id);

    // 通用的线程池管理线程开启工作线程管理任务
    pthread_t ThreadPoolManagerThread_id;
    ThreadPool *pThreadPool = CreateThreadPool(function);
    // 配置结构体
    ThreadPoolManagerConfig *pConfig = (ThreadPoolManagerConfig *)malloc(sizeof(ThreadPoolManagerConfig));
    pConfig->pool = pThreadPool;
    pConfig->maxThreads = 256;
    pConfig->minIdleThreads = 8;
    pConfig->releasePeriod = 60;
    pConfig->workFunction = handle_http_request;
    pConfig->workFunctionArgs = NULL;

    pthread_create(&ThreadPoolManagerThread_id, NULL, ThreadPoolManagerThread, pConfig);
    pthread_detach(ThreadPoolManagerThread_id);

    int port = 8080;
    int max_backlog = 10;

    ServerConfig *pServerConfig = (ServerConfig *)malloc(sizeof(ServerConfig));
    InitServerConfig(pServerConfig);
    pServerConfig->port = port;
    pServerConfig->max_backlog = max_backlog;

    // 启动SocketServer线程
    pthread_t SocketServerThread_id;
    pthread_create(&SocketServerThread_id, NULL, SocketServerThread, pServerConfig);
    pthread_detach(SocketServerThread_id);


    printf("Web server is running on port %d\n", port);

    while(1) {

        printf("请输入操作：");

        char str[1024];
        scanf("%s", str);

        if(strcasecmp("status", str) == 0) {
            PrintSocketServerStatus(pServerConfig);
            PrintTCPPoolStatus(pTCPPOOL);
            PrintThreadPoolStatus(pThreadPool);
            // 后续还可以继续拓展
        }
        if(strcasecmp("exit", str) == 0) {
            keep_running = 0;
            // 等待线程退出
            pthread_join(timer_thread_id, NULL);
            pthread_join(tcppool_thread_id, NULL);
            pthread_join(ThreadPoolManagerThread_id, NULL);
            pthread_join(SocketServerThread_id, NULL);

            break;
        }
    }

    // 释放全局变量
    FreeSocketQueue(ActiveSocketQueue);
    FreeSocketQueue(IdleSocketQueue);

    return 0;
}

void handle_http_request() {

    // 直接从队列里面取
    int client_socket = GetFromSocketQueue(ActiveSocketQueue);
    if(client_socket == -1) {
        // 获取失败退出
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
                        close(client_socket);
                        return;
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
                        close(client_socket);
                        return;
                    }
                }
            } else {
                char *error_message = "501 Not Implemented";
                sprintf(response, "HTTP/1.1 501 Not Implemented\r\nConnection: %s\r\n\r\n%s", keep_alive ? "keep-alive" : "close", error_message);
                if (write(client_socket, response, strlen(response)) == -1) {
                    perror("response_head");
                    close(client_socket);
                    return;
                }
            }

            // 判断是否还需要保持连接，如果不是，则退出循环，也不要再放入连接池了，以用户要求为准
            if (!keep_alive) {
                close(client_socket);
                return;
            }
        } else if (bytes_read == 0) {
            // 读取到EOF，客户端关闭连接
            close(client_socket);
            return;
        } else {
            // 如果没有读取到数据，并且不是因为阻塞，说明出错
            if (errno != EAGAIN || errno != EWOULDBLOCK) {
                perror("read error");
            } else {
                // 加入空闲队列
                AddToSocketQueue(IdleSocketQueue, client_socket);
            }
            return;
        }
    }
}
