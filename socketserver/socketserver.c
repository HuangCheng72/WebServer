//
// Created by huangcheng on 2024/5/9.
//

#include "socketserver.h"

// 活跃队列，就是有任务的，刚进来的肯定要默认为有任务的，所以只能进入活跃队列
extern SocketTaskQueue* ActiveSocketQueue;

// 程序是否继续运行标志
extern int keep_running;

void InitServerConfig(ServerConfig *pConfig) {
    pConfig->server_running = 0; // 初始状态设为停止
    pthread_mutex_init(&pConfig->state_mutex, NULL); // 初始化互斥锁
}

void FreeServerConfig(ServerConfig *pConfig) {
    pthread_mutex_destroy(&pConfig->state_mutex); // 销毁互斥锁
    free(pConfig);
}

// 设置文件为非阻塞模式
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

// 专门用于Socket的线程
void *SocketServerThread(void *arg) {
    if(!arg) pthread_exit(NULL);

    ServerConfig *config = (ServerConfig *)arg;

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
    server_addr.sin_port = htons(config->port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Socket bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, config->max_backlog) < 0) {
        perror("Socket listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 一切成功，运行状态修改
    pthread_mutex_lock(&config->state_mutex);
    config->server_running = 1;
    pthread_mutex_unlock(&config->state_mutex);

    while (1 && keep_running) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_socket < 0) {
            perror("Client accept failed");
            continue;
        }
        //将client_socket设为非阻塞模式
        set_non_blocking(client_socket);
        AddToSocketQueue(ActiveSocketQueue, client_socket);
    }

    // 运行状态修改
    pthread_mutex_lock(&config->state_mutex);
    config->server_running = 0;
    pthread_mutex_unlock(&config->state_mutex);

    close(server_socket);

    FreeServerConfig(config);

    pthread_exit(NULL);
}

void PrintSocketServerStatus(ServerConfig *pConfig) {
    if(!pConfig) return;
    pthread_mutex_lock(&pConfig->state_mutex);
    printf("端口 %d 监听状态: %s\n", pConfig->port, pConfig->server_running ? "正在运行中···" : "没有监听");
    pthread_mutex_unlock(&pConfig->state_mutex);
}
