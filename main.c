#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
#include "list/RequestQueue.h"

// 随便一个端口都行，只要没占用、没被防火墙拦截（所以需要在防火墙里面放行）就行。
#define PORT 8080
// 这里我需要解释一下，"/home/hc/web"是我的网站文件夹目录，可以随便修改，或者改成用参数接受不同的路径也可以。
#define WEB_ROOT "/home/hc/web"

// 活跃队列，就是有任务的
SocketTaskQueue* ActiveSocketQueue = NULL;
// 空闲队列，就是没任务的
SocketTaskQueue* IdleSocketQueue = NULL;

// HTTP请求接收函数（管道化）
void receive_http_requests_pipelining(int clientSocket, RequestQueue *pRequestQueue);

// 解析与响应函数（管道化）
void parse_and_respond_pipelining(int clientSocket, RequestQueue *pRequestQueue);

// HTTP请求处理函数（管道化）
void handle_http_request_pipelining();

// 用于判断是否有任务需要处理的函数
int function() { return !IsSocketQueueEmpty(ActiveSocketQueue); }

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1; // 处理错误
    }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1) {
        return -1; // 处理错误
    }
    return 0;
}

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Socket bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 10) < 0) {
        perror("Socket listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 设置server_socket非阻塞模式
    if (set_non_blocking(server_socket) < 0) {
        perror("Failed to set server socket to non-blocking");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

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
    pConfig->minIdleThreads = 10;
    pConfig->releasePeriod = 60;
    pConfig->workFunction = handle_http_request_pipelining;
    pConfig->workFunctionArgs = NULL;

    pthread_create(&ThreadPoolManagerThread_id, NULL, ThreadPoolManagerThread, pConfig);
    pthread_detach(ThreadPoolManagerThread_id);

    printf("Web server is running on port %d\n", PORT);

    while (1) {

        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_socket > 0) {
            // 一开始就设置为非阻塞模式
            set_non_blocking(client_socket);
            // 先进入活跃队列
            AddToSocketQueue(ActiveSocketQueue, client_socket);
        } else if (client_socket < 0) {
            if (errno != EAGAIN || errno != EWOULDBLOCK) {
                perror("Client accept failed");
                continue;
            }
        }
    }

    close(server_socket);
    return 0;
}

void receive_http_requests_pipelining(int clientSocket, RequestQueue *pRequestQueue) {
    char buffer[8192];
    char request[8192];  // 用于存储一个完整的请求
    int request_len = 0; // 当前请求长度

    while (1) {
        int bytes_read = read(clientSocket, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';  // 确保字符串正确终止
            int buffer_pos = 0; // 用于遍历buffer

            // 处理buffer中的数据
            while (buffer_pos < bytes_read) {
                char *end_of_request = strstr(&buffer[buffer_pos], "\r\n\r\n"); // 查找请求结束标志
                if (end_of_request != NULL) {
                    int length_to_end_of_request = end_of_request - &buffer[buffer_pos] + 4; // 包括"\r\n\r\n"
                    // 检查是否有足够空间拼接完整请求
                    if (request_len + length_to_end_of_request < sizeof(request)) {
                        // 拼接当前部分到请求
                        memcpy(request + request_len, &buffer[buffer_pos], length_to_end_of_request);
                        request_len += length_to_end_of_request;

                        // 将完整请求添加到队列
                        AddToRequestQueue(pRequestQueue, request, request_len);

                        // 清空request，准备下一个请求
                        memset(request, 0, sizeof(request));
                        request_len = 0;

                        // 移动buffer_pos到下一个可能的请求起始位置
                        buffer_pos += length_to_end_of_request;
                    } else {
                        // 如果请求太长无法处理，可以返回错误或者断开连接
                        break;
                    }
                } else {
                    // 如果没找到请求结束，将剩余部分添加到request
                    int remaining_length = bytes_read - buffer_pos;
                    if (request_len + remaining_length < sizeof(request)) {
                        memcpy(request + request_len, &buffer[buffer_pos], remaining_length);
                        request_len += remaining_length;
                    } else {
                        // 如果请求太长无法处理，可以返回错误或者断开连接
                        break;
                    }
                    break; // 等待下一次读取
                }
            }
        } else if (bytes_read == 0) {
            // 读取到EOF，客户端关闭连接
            break;
        } else {
            // 读取错误处理
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read error");
                break;
            }
            // 读取完成但是没有错误，退出循环
            break;
        }
    }
}

void parse_and_respond_pipelining(int clientSocket, RequestQueue *pRequestQueue) {

    if(clientSocket == -1 || !pRequestQueue) {
        return;
    }
    int client_socket = clientSocket;

    while (!IsRequestQueueEmpty(pRequestQueue)) {
        RequestData *data = GetFromRequestQueue(pRequestQueue);
        if(data == NULL) {
            return;
        }

        // 这里处理每一个独立的HTTP请求
        char method[10], path[1024], protocol[10];
        if (sscanf(data->data, "%s %s %s", method, path, protocol) == 3) {
            //这个变量是用于确定还要不要继续连接下去
            int keep_alive = 1; //HTTP/1.1默认就是持久连接

            if(strcasecmp("HTTP/1.0", protocol) == 0) {
                keep_alive = 0; //HTTP/1.0默认没有持久连接
            }

            char *connection_header = strstr(data->data, "Connection:");

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
                // 这里就要关闭了TCP连接了
                close(client_socket);
                client_socket = -1;
                FreeRequestData(data);  // 释放已处理的请求数据
                break;
            }
        }

        FreeRequestData(data);  // 释放已处理的请求数据
    }

    if(client_socket > -1) {
        // TCP连接没关闭送回空闲队列
        AddToSocketQueue(IdleSocketQueue, client_socket);
    }
}

void handle_http_request_pipelining() {

    // 直接从队列里面取
    int client_socket = GetFromSocketQueue(ActiveSocketQueue);
    if(client_socket == -1) {
        // 获取失败退出
        return;
    }

    RequestQueue *pRequestQueue = CreateRequestQueue();

    //接收数据
    receive_http_requests_pipelining(client_socket, pRequestQueue);

    //响应和处理数据
    parse_and_respond_pipelining(client_socket, pRequestQueue);
}
