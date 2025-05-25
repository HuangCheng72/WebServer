//
// Created by huangcheng on 2025/5/21.
//

/**
 * daemon的主要作用就是作为守护进程。
 * 它的作用很简单：建立各进程之间的通信、启动各进程。
 */

#include "transfer_fd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <ftw.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>

// 是否保持运行的flag
int keep_running;

// 守护进程与监听进程之间的socket对（分配规则，左边分到sock1，右边分到sock2）
SOCKETPAIR daemon_listener;
SOCKETPAIR daemon_manager;

// 监听进程和管理进程之间的socket对（用两个是为了特化用途，一个发一个收，一个线程监视一个，这样不需要去做判断，提高处理效率）
SOCKETPAIR listener_send_manager_recv;
SOCKETPAIR listener_recv_manager_send;

// 监听进程的status
StatusMessage listener_status;
// 管理进程的status
StatusMessage manager_status;

void *ListenerMonitorThread(void *arg) {
    struct timespec last_heartbeat_time = {0};
    struct timespec last_restart_time = {0};
    struct timespec last_timeout_warn_time = {0};

    const int heartbeat_timeout_ms = 5000;  // 心跳超时时间
    const int warn_interval_ms = 5000;      // 超时警告打印间隔
    const int max_warn_times = 3;           // 最多打印3次
    const int restart_interval_ms = 5000;   // 两次重启之间的间隔至少为五秒

    int timeout_warn_count = 0;             // 已经超时警告次数

    int daemon_sock = daemon_listener.sock1;
    int listener_sock = daemon_listener.sock2;

    StatusMessage tmp;

    pid_t pid;

    // 初次启动（不然会出现一开始收不到心跳包，多次重启的情况）

    // fork前准备工作，把要给监听进程的sock发送过去
    // 因为这一对是全双工的，所以我从守护进程持有的这一端发过去，监听进程就能接收到
    // 监听进程和管理进程之间的socket对，listener根据规则持有sock1，manager根据规则持有sock2
    fd_send(daemon_sock, listener_send_manager_recv.sock1);
    fd_send(daemon_sock, listener_recv_manager_send.sock1);

    pid = fork();
    if (pid < 0) {
        perror("[ERROR] Failed to fork");
        return NULL;
    }

    if (pid == 0) {
        // pid == 0时，此时是在子进程中了
        // fork+exec 方式启动进程，可以保留文件描述符资源，因此可以直接把socketpair中属于功能进程的那一端作为启动参数传过去

        // 子进程作用：启动 Listener 服务

        // 使用listener_sock作为参数来启动子进程
        char fd_str[32];
        snprintf(fd_str, sizeof(fd_str), "%d", listener_sock);  // 将文件描述符转为字符串

        // 用execl来启动功能进程
        execl("./Listener", "WebServer_Listener", fd_str, NULL);
        // 如果 execl 返回，表示执行失败
        perror("[ERROR] Failed to exec Listener");

        exit(1);    // 这里退出的是子进程

    } else {
        // 父进程：监控子进程的状态
        listener_status.module = 1;
        listener_status.pid = pid;
        printf("[INFO] Listener process started with PID: %d\n", pid);
    }

    // 停顿五秒，让系统完成进程启动工作
    sleep(5);
    clock_gettime(CLOCK_MONOTONIC, &last_restart_time);

    while (keep_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // === 使用 select 等待可读 ===
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(daemon_sock, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(daemon_sock + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳过本轮
            goto check_timeout_and_restart;
        }

        // 有数据可读
        int ret = recv_status_message(daemon_sock, &tmp);
        if (ret == 0) {
            memcpy(&listener_status, &tmp, sizeof(StatusMessage));
            clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
            timeout_warn_count = 0;     // 收到心跳包就清空超时警告
        }

check_timeout_and_restart:
        // 心跳超时判断
        if (listener_status.pid > 0) {
            long delta_ms =
                    (now.tv_sec - last_heartbeat_time.tv_sec) * 1000 +
                    (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000000;

            if (delta_ms > heartbeat_timeout_ms) {

                // 检查是否到达下一个 warning 输出时间
                long since_last_warn = (now.tv_sec - last_timeout_warn_time.tv_sec) * 1000 +
                                       (now.tv_nsec - last_timeout_warn_time.tv_nsec) / 1000000;

                if (since_last_warn >= warn_interval_ms) {
                    printf("[WARN] Listener heartbeat timeout #%d. (PID: %d)\n",
                           timeout_warn_count + 1, listener_status.pid);
                    clock_gettime(CLOCK_MONOTONIC, &last_timeout_warn_time);
                    timeout_warn_count++;

                    if (timeout_warn_count == max_warn_times) {
                        // 打出重启标志，重启
                        listener_status.module = -10;
                        timeout_warn_count = 0;     // 清零超时警告次数
                    }
                }
            }
        }

        // 检查是否需要重启监听进程
        if (listener_status.module == -10) {
            long delta_ms =
                    (now.tv_sec - last_restart_time.tv_sec) * 1000 +
                    (now.tv_nsec - last_restart_time.tv_nsec) / 1000000;

            // 如果超过重启间隔，就重启监听进程
            if (delta_ms > restart_interval_ms) {

                if (listener_status.pid > 0) {
                    // 确保杀死老进程（先发送shutdown的msg，如果没反应就kill）

                    StatusMessage shutdown_msg;
                    // 这就是几个随便乱选的Magic Number
                    shutdown_msg.args[0] = -25;
                    shutdown_msg.args[3] = -41;
                    shutdown_msg.args[10] = -99;
                    send_status_message(daemon_sock, &shutdown_msg);

                    // 等待一段时间，让 Listener 自己退出
                    int wait_time_ms = 5000;
                    for (int i = 0; i < wait_time_ms / 100; i++) {
                        // 用waitpid来等待Listener自己退出
                        if (waitpid(listener_status.pid, NULL, WNOHANG) > 0) {
                            printf("[INFO] Listener exited.\n");

                            // Listener自己退出，就开始做重启工作
                            goto Listener_do_restart;
                        }
                        usleep(100 * 1000);
                    }

                    // Listener超时无法自己退出，强制杀掉
                    printf("[WARN] Listener did not exit, sending SIGKILL...\n");
                    kill(listener_status.pid, SIGKILL);
                    waitpid(listener_status.pid, NULL, 0);
                    // 清理状态
                    memset(&listener_status, -1, sizeof(StatusMessage));

                }

Listener_do_restart:
                printf("[INFO] Restarting Listener...\n");

                // fork前准备工作，把要给监听进程的sock发送过去
                // 因为这一对是全双工的，所以我从守护进程持有的这一端发过去，监听进程就能接收到
                // 监听进程和管理进程之间的socket对，listener根据规则持有sock1，manager根据规则持有sock2
                fd_send(daemon_sock, listener_send_manager_recv.sock1);
                fd_send(daemon_sock, listener_recv_manager_send.sock1);

                // 重启监听进程
                pid = fork();
                if (pid < 0) {
                    perror("[ERROR] Failed to fork");
                    break;
                }

                if (pid == 0) {

                    // 在子进程中启动监听进程
                    char fd_str[32];
                    snprintf(fd_str, sizeof(fd_str), "%d", listener_sock);  // 将文件描述符转为字符串
                    execl("./Listener", "WebServer_Listener", fd_str, NULL);
                    // 如果 execl 返回，表示执行失败
                    perror("[ERROR] Failed to exec Listener");
                    exit(1);  // 子进程退出
                } else {
                    // 更新父进程中的PID
                    listener_status.module = 1;
                    listener_status.pid = pid;
                    clock_gettime(CLOCK_MONOTONIC, &last_restart_time);  // 更新重启时间
                    printf("[INFO] Listener process restarted with PID: %d\n", pid);
                }
            }
        }

        usleep(200 * 1000);  // 200ms 轮询间隔
    }

    if (listener_status.pid > 0) {
        // 确保杀死老进程（先发送shutdown的msg，如果没反应就kill）

        StatusMessage shutdown_msg;
        // 这就是几个随便乱选的Magic Number
        shutdown_msg.args[0] = -25;
        shutdown_msg.args[3] = -41;
        shutdown_msg.args[10] = -99;
        send_status_message(daemon_sock, &shutdown_msg);

        // 等待一段时间，让 Listener 自己退出
        int wait_time_ms = 5000;
        for (int i = 0; i < wait_time_ms / 100; i++) {
            // 用waitpid来等待Listener自己退出
            if (waitpid(listener_status.pid, NULL, WNOHANG) > 0) {
                printf("[INFO] Listener exited.\n");
                pthread_exit(NULL);
            }
            usleep(100 * 1000);
        }

        // Listener超时无法自己退出，强制杀掉
        printf("[WARN] Listener did not exit, sending SIGKILL...\n");
        kill(listener_status.pid, SIGKILL);
        waitpid(listener_status.pid, NULL, 0);
        // 清理状态
        memset(&listener_status, -1, sizeof(StatusMessage));

    }

    pthread_exit(NULL);
}

void *ManagerMonitorThread(void *arg) {
    struct timespec last_heartbeat_time = {0};
    struct timespec last_restart_time = {0};
    struct timespec last_timeout_warn_time = {0};

    const int heartbeat_timeout_ms = 5000;  // 心跳超时时间
    const int warn_interval_ms = 5000;      // 超时警告打印间隔
    const int max_warn_times = 3;           // 最多打印3次
    const int restart_interval_ms = 5000;   // 两次重启之间的间隔至少为五秒

    int timeout_warn_count = 0;             // 已经超时警告次数


    int daemon_sock = daemon_manager.sock1;
    int manager_sock = daemon_manager.sock2;

    // 监听进程和管理进程之间的socket对，listener根据规则持有sock1，manager根据规则持有sock2

    StatusMessage tmp;

    pid_t pid;

    // 初次启动（不然会出现一开始收不到心跳包，多次重启的情况）

    // fork前准备工作，把要给监听进程的sock发送过去
    // 因为这一对是全双工的，所以我从守护进程持有的这一端发过去，监听进程就能接收到
    // 监听进程和管理进程之间的socket对，listener根据规则持有sock1，manager根据规则持有sock2
    fd_send(daemon_sock, listener_recv_manager_send.sock2);
    fd_send(daemon_sock, listener_send_manager_recv.sock2);


    pid = fork();
    if (pid < 0) {
        perror("[ERROR] Failed to fork");
        return NULL;
    }

    if (pid == 0) {
        // pid == 0时，此时是在子进程中了
        // fork+exec 方式启动进程，可以保留文件描述符资源，因此可以直接把socketpair中属于功能进程的那一端作为启动参数传过去

        // 子进程作用：启动 Manager 服务

        // 使用manager_sock作为参数来启动子进程
        char fd_str[32];
        snprintf(fd_str, sizeof(fd_str), "%d", manager_sock);  // 将文件描述符转为字符串

        // 用execl来启动功能进程
        execl("./Manager", "WebServer_Manager", fd_str, NULL);
        // 如果 execl 返回，表示执行失败
        perror("[ERROR] Failed to exec Manager");

        exit(1);    // 这里退出的是子进程

    } else {
        // 父进程：监控子进程的状态
        printf("[INFO] Manager process started with PID: %d\n", pid);
        manager_status.module = 2;
        manager_status.pid = pid;
    }

    // 停顿五秒，让系统完成进程启动工作
    sleep(5);
    clock_gettime(CLOCK_MONOTONIC, &last_restart_time);

    while (keep_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // === 使用 select 等待可读 ===
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(daemon_sock, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(daemon_sock + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳过本轮
            goto check_timeout_and_restart;
        }

        // 有数据可读
        int ret = recv_status_message(daemon_sock, &tmp);
        if (ret == 0) {
            memcpy(&manager_status, &tmp, sizeof(StatusMessage));
            clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
        }

check_timeout_and_restart:
        // 心跳超时判断
        if (manager_status.pid > 0) {
            long delta_ms =
                    (now.tv_sec - last_heartbeat_time.tv_sec) * 1000 +
                    (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000000;

            if (delta_ms > heartbeat_timeout_ms) {
                // 检查是否到达下一个 warning 输出时间
                long since_last_warn = (now.tv_sec - last_timeout_warn_time.tv_sec) * 1000 +
                                       (now.tv_nsec - last_timeout_warn_time.tv_nsec) / 1000000;

                if (since_last_warn >= warn_interval_ms) {
                    printf("[WARN] Manager heartbeat timeout #%d. (PID: %d)\n",
                           timeout_warn_count + 1, manager_status.pid);
                    clock_gettime(CLOCK_MONOTONIC, &last_timeout_warn_time);
                    timeout_warn_count++;

                    if (timeout_warn_count == max_warn_times) {
                        // 打出重启标志，重启
                        manager_status.module = -20;
                        timeout_warn_count = 0;     // 清零超时警告次数
                    }
                }
            }
        }

        // 检查是否需要重启管理进程
        if (manager_status.module == -20) {
            long delta_ms =
                    (now.tv_sec - last_restart_time.tv_sec) * 1000 +
                    (now.tv_nsec - last_restart_time.tv_nsec) / 1000000;

            // 如果超过重启间隔，就重启管理进程
            if (delta_ms > restart_interval_ms) {

                if (manager_status.pid > 0) {
                    // 确保杀死老进程（先发送shutdown的msg，如果没反应就kill）
                    StatusMessage shutdown_msg;
                    // 这就是几个随便乱选的Magic Number
                    shutdown_msg.args[0] = -25;
                    shutdown_msg.args[3] = -41;
                    shutdown_msg.args[10] = -99;
                    send_status_message(daemon_sock, &shutdown_msg);

                    // 等待一段时间，让 Manager 自己退出
                    int wait_time_ms = 5000;
                    for (int i = 0; i < wait_time_ms / 100; i++) {
                        // 用waitpid来等待Manager自己退出
                        if (waitpid(manager_status.pid, NULL, WNOHANG) > 0) {
                            printf("[INFO] Manager exited.\n");

                            // Manager自己退出，就开始做重启工作
                            goto Manager_do_restart;
                        }
                        usleep(100 * 1000);
                    }

                    // Listener超时无法自己退出，强制杀掉
                    printf("[WARN] Manager did not exit, sending SIGKILL...\n");
                    kill(manager_status.pid, SIGKILL);
                    waitpid(manager_status.pid, NULL, 0);
                    // 清理状态
                    memset(&manager_status, -1, sizeof(StatusMessage));

                }

Manager_do_restart:

                printf("[INFO] Restarting Manager...\n");

                // fork前准备工作，把要给管理进程的sock发送过去
                // 因为这一对是全双工的，所以我从守护进程持有的这一端发过去，管理进程就能接收到
                fd_send(daemon_sock, listener_recv_manager_send.sock2);
                fd_send(daemon_sock, listener_send_manager_recv.sock2);

                // 重启管理进程
                pid = fork();
                if (pid < 0) {
                    perror("[ERROR] Failed to fork");
                    break;
                }

                if (pid == 0) {
                    // 在子进程中启动管理进程
                    char fd_str[32];
                    snprintf(fd_str, sizeof(fd_str), "%d", manager_sock);  // 将文件描述符转为字符串
                    execl("./Manager", "WebServer_Manager", fd_str, NULL);
                    // 如果 execl 返回，表示执行失败
                    perror("[ERROR] Failed to exec Manager");
                    exit(1);  // 子进程退出
                } else {
                    // 更新父进程中的PID
                    manager_status.module = 2;
                    manager_status.pid = pid;
                    clock_gettime(CLOCK_MONOTONIC, &last_restart_time);  // 更新重启时间
                    printf("[INFO] Manager process restarted with PID: %d\n", pid);
                }
            }
        }

        usleep(200 * 1000);  // 200ms 轮询间隔
    }

    if (manager_status.pid > 0) {
        // 确保杀死老进程（先发送shutdown的msg，如果没反应就kill）
        StatusMessage shutdown_msg;
        // 这就是几个随便乱选的Magic Number
        shutdown_msg.args[0] = -25;
        shutdown_msg.args[3] = -41;
        shutdown_msg.args[10] = -99;
        send_status_message(daemon_sock, &shutdown_msg);

        // 等待一段时间，让 Manager 自己退出
        int wait_time_ms = 5000;
        for (int i = 0; i < wait_time_ms / 100; i++) {
            // 用waitpid来等待Manager自己退出
            if (waitpid(manager_status.pid, NULL, WNOHANG) > 0) {
                printf("[INFO] Manager exited.\n");
                pthread_exit(NULL);
            }
            usleep(100 * 1000);
        }

        // Manager超时无法自己退出，强制杀掉
        printf("[WARN] Manager did not exit, sending SIGKILL...\n");
        kill(manager_status.pid, SIGKILL);
        waitpid(manager_status.pid, NULL, 0);
        // 清理状态
        memset(&manager_status, -1, sizeof(StatusMessage));

    }

    pthread_exit(NULL);
}

/**
 * 通用状态打印函数
 * @param status 传入状态指针
 * @param arg_count 参数个数
 * @param ...
 */
void print_status_table(const StatusMessage *status, int arg_count, ...) {
    if (!status || arg_count < 0) {
        printf("[ERROR] Invalid arguments to print_status_table.\n");
        return;
    }

    printf("+-------------------+------------------+\n");
    printf("| %-17s | %-16s |\n", "Field", "Value");
    printf("+-------------------+------------------+\n");

    // 打印固定字段
    printf("| %-17s | %-16d |\n", "Module", status->module);
    printf("| %-17s | %-16d |\n", "PID", status->pid);

    // 打印可变字段
    va_list args;
    va_start(args, arg_count);
    for (int i = 0; i < arg_count; ++i) {
        const char *label = va_arg(args, const char *);
        printf("| %-17s | %-16d |\n", label, status->args[i]);
    }
    va_end(args);

    printf("+-------------------+------------------+\n");
}

int main() {
    keep_running = 1;

    socketpair_create(&daemon_listener);
    socketpair_create(&daemon_manager);

    socketpair_create(&listener_send_manager_recv);
    socketpair_create(&listener_recv_manager_send);

    // 初始化监听进程状态
    memset(&listener_status, -1, sizeof(StatusMessage));
    memset(&manager_status, -1, sizeof(StatusMessage));

    printf("[INFO] WebServer_Daemon_Main started.\n");

    // 专门负责更新监视listener的情况，接收心跳信息包，更新状态信息
    pthread_t listener_monitor_thread;
    pthread_create(&listener_monitor_thread, NULL, ListenerMonitorThread, NULL);

    // 专门负责更新监视listener的情况，接收心跳信息包，更新状态信息
    pthread_t manager_monitor_thread;
    pthread_create(&manager_monitor_thread, NULL, ManagerMonitorThread, NULL);

    // 主线程只负责用来等待用户输入指令，以及做出各种反应
    char command[128] = {0};
    while(keep_running) {
        printf("> ");
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) {
            continue;
        }

        // 去除换行符
        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "status") == 0) {
            printf("Listener Status:\n");
            print_status_table(&listener_status, 5,
                               "Pool Size",
                               "Pool Count",
                               "Top Timeout",
                               "Accept Queue",
                               "Release Queue"
            );

            printf("Manager Status:\n");
            print_status_table(&manager_status, 0); // 若 manager 没有额外字段也可以打印模块与PID

        } else if (strcmp(command, "restart") == 0) {
            printf("Forcing restart...\n");
            listener_status.module = -10;   // 强制触发重启
            manager_status.module = -20;    // 强制触发重启
        } else if (strcmp(command, "exit") == 0) {
            printf("Shutting down...\n");

            // 向 Listener 发送关闭指令
            StatusMessage shutdown_msg;
            // 这就是几个随便乱选的Magic Number
            shutdown_msg.args[0] = -25;
            shutdown_msg.args[3] = -41;
            shutdown_msg.args[10] = -99;
            send_status_message(daemon_listener.sock1, &shutdown_msg);

            // 向 Manager 发送关闭指令
            send_status_message(daemon_manager.sock1, &shutdown_msg);

            keep_running = 0;
            break;
        } else if (strcmp(command, "") == 0) {
            // 没意义，跳过
            continue;
        } else {
            printf("Unknown command: %s\n", command);
        }
    }

    // 等待监控线程全部退出
    pthread_join(listener_monitor_thread, NULL);
    pthread_join(manager_monitor_thread, NULL);


    // 销毁的资源
    socketpair_destroy(&daemon_listener);
    socketpair_destroy(&daemon_manager);

    socketpair_destroy(&listener_send_manager_recv);
    socketpair_destroy(&listener_recv_manager_send);

    // 各个功能进程确实退出之后再打印这句
    printf("[INFO] WebServer_Daemon_Main exit.\n");

    return 0;
}
