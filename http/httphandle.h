//
// Created by huangcheng on 2024/5/6.
//

#ifndef WEBSERVER_HTTPHANDLE_H
#define WEBSERVER_HTTPHANDLE_H

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

typedef struct {
    // 网站文件夹的绝对路径
    char root[2048];
    // 请求行，三要素
    char method[10];
    char path[1024];
    char protocol[10];
    // 请求头其他字段，可以根据自己的要求扩展
    int keep_alive;
} HttpRequest;

/**
 * 解析HTTP请求
 * @param buffer 报文
 * @param request 结构化的解析结果
 * @return 成功返回1，失败返回0
 */
int parse_request(char *buffer, HttpRequest *request);

/**
 * 解析GET请求
 * @param client_socket 代表用户的TCP连接（用文件描述符表示）
 * @param request 结构化的解析结果
 */
void handle_get_request(int client_socket, HttpRequest *request);

/**
 * 通用的错误处理
 * @param client_socket 代表用户的TCP连接（用文件描述符表示）
 * @param request 结构化的解析结果
 * @param status_code 状态码
 * @param message 状态信息
 */
void handle_error_method(int client_socket, HttpRequest *request, const char *status_code, const char *message);

#endif //WEBSERVER_HTTPHANDLE_H
