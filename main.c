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

// 随便一个端口都行，只要没占用、没被防火墙拦截（所以需要在防火墙里面放行）就行。
#define PORT 8080
// 这里我需要解释一下，"/home/hc/web"是我的网站文件夹目录，可以随便修改，或者改成用参数接受不同的路径也可以。
#define WEB_ROOT "/home/hc/web"
// 设定超时时间为60秒
#define TIMEOUT_SECONDS 60

void *handle_http_request(void *p_client_socket);

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
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 10) < 0) {
        perror("Socket listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Web server is running on port %d\n", PORT);

    // 开启计时线程
    pthread_t timer_thread_id;
    pthread_create(&timer_thread_id, NULL, timer_thread, NULL);
    pthread_detach(timer_thread_id);

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Client accept failed");
            continue;
        }

        // 一开始就设置为非阻塞模式
        set_non_blocking(client_socket);

        pthread_t thread_id;
        int *pclient = malloc(sizeof(int));
        *pclient = client_socket;
        //创建Linux线程
        pthread_create(&thread_id, NULL, handle_http_request, pclient);
        //把线程加入Linux的线程队列，由Linux系统调度
        pthread_detach(thread_id);


        //需要说明，我的这个WebServer现在使用多线程不加锁
        //是因为每个线程的没有共享资源，根本不冲突，每个线程都是服务不同的用户连接
        //这种情况下加锁就没有意义了，还增加开销
    }

    close(server_socket);
    return 0;
}

void *handle_http_request(void *p_client_socket) {
    int client_socket = *((int *)p_client_socket);
    free(p_client_socket);

    char buffer[4096];

    // 注册当前线程到计时管理线程
    TimerThreadControl *timer_ctrl = register_timer_thread(pthread_self());
    if (!timer_ctrl) {
        close(client_socket);
        pthread_exit(NULL);
    }

    int count = TIMEOUT_SECONDS; //计数器一开始计为设定的超时值

    while (1) {

        // 等待计时器线程的信号，阻塞时间极短，可以认为是非阻塞方式
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // 获取当前时间

        pthread_mutex_lock(timer_ctrl->mutex);
        // 设置超时时间为当前时间，即不等待
        int wait_result = pthread_cond_timedwait(timer_ctrl->cond, timer_ctrl->mutex, &now);
        pthread_mutex_unlock(timer_ctrl->mutex);

        if (wait_result == 0) {
            count--; // 仅在成功接收到信号时递减计数器
            if (count <= 0) {
                // 超时退出循环，准备关闭连接
                break;
            }
        } else if (wait_result == ETIMEDOUT) {
            // 没有收到信号，继续执行
            // 确定是否有数据到达，有就处理
            int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
            if(bytes_read > 0) {
                // 重置计数器
                count = TIMEOUT_SECONDS;
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
                            pthread_exit(NULL);
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
                            pthread_exit(NULL);
                        }
                    }
                } else {
                    char *error_message = "501 Not Implemented";
                    sprintf(response, "HTTP/1.1 501 Not Implemented\r\nConnection: %s\r\n\r\n%s", keep_alive ? "keep-alive" : "close", error_message);
                    if (write(client_socket, response, strlen(response)) == -1) {
                        perror("response_head");
                        close(client_socket);
                        pthread_exit(NULL);
                    }
                }

                // 判断是否还需要保持连接，如果不是，则退出循环
                if (!keep_alive) {
                    break;
                }
            } else if (bytes_read == 0) {
                // 读取到EOF，客户端关闭连接
                break;
            } else {
                // 如果没有读取到数据，并且不是因为阻塞，说明出错
                if (errno != EAGAIN || errno != EWOULDBLOCK) {
                    perror("read error");
                    break;
                }
            }
        }
    }
    // 准备退出线程，关闭连接，取消订阅计时线程
    close(client_socket);
    unregister_timer_thread(timer_ctrl);
    pthread_exit(NULL);
}