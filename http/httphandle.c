//
// Created by huangcheng on 2024/5/6.
//

#include "httphandle.h"

// 解析HTTP请求
int parse_request(char *buffer, HttpRequest *request) {
    char *connection_header;
    if (sscanf(buffer, "%s %s %s", request->method, request->path, request->protocol) != 3) {
        return 0;  // 解析失败
    }
    request->keep_alive = (strcasecmp("HTTP/1.0", request->protocol) == 0) ? 0 : 1;
    connection_header = strstr(buffer, "Connection:");
    if (connection_header && strstr(connection_header, "keep-alive")) {
        request->keep_alive = 1;
    }
    return 1;  // 解析成功
}

// 处理GET请求
void handle_get_request(int client_socket, HttpRequest *request) {
    char response[4096], full_path[2048];
    sprintf(full_path, "%s%s", request->root, strcmp(request->path, "/") == 0 ? "/index.html" : request->path);
    int file = open(full_path, O_RDONLY);
    if (file != -1) {
        struct stat stat_buf;
        fstat(file, &stat_buf);
        sprintf(response, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: %s\r\n\r\n", stat_buf.st_size, request->keep_alive ? "keep-alive" : "close");
        write(client_socket, response, strlen(response));
        sendfile(client_socket, file, NULL, stat_buf.st_size);
        close(file);
    } else {
        handle_error_method(client_socket, request, "404 Not Found", "The requested URL was not found on this server.");
    }
}

// 处理出错的通用方法
void handle_error_method(int client_socket, HttpRequest *request, const char *status_code, const char *message) {
    char response[4096];
    sprintf(response, "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s", status_code, strlen(message), request->keep_alive ? "keep-alive" : "close", message);
    write(client_socket, response, strlen(response));
}