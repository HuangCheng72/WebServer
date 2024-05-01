#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define WEB_ROOT "/home/hc/web"

void handle_http_request(int client_socket);

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

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Client accept failed");
            continue;
        }

        handle_http_request(client_socket);
        close(client_socket);
    }

    close(server_socket);
    return 0;
}

void handle_http_request(int client_socket) {
    char buffer[4096];
    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);   //从代表客户端的文件（套接字描述符指向的文件）中读取数据
    buffer[bytes_read] = '\0';  //这是为了保证最后一个字符一定是\0，字符串不会溢出

    char method[10], path[1024], protocol[10];
    sscanf(buffer, "%s %s %s", method, path, protocol); //从缓冲区读入第一行请求的三个要素：请求方法，请求路径，协议

    // 响应头和响应内容
    char response[4096];
    char content[2048];
    int content_length = 0;

    // 拼接完整路径
    char full_path[1024];
    sprintf(full_path, "%s%s", WEB_ROOT, strcmp(path, "/") == 0 ? "/index.html" : path);    //因为/这种默认的情况一般都处理的是index，所以把这种情况默认处理了

    // 如果不是PUT和DELETE，要操作文件，权限只能是读，不能是其他
    // 所以，如果文件不存在，那file必然是0
    FILE *file = NULL;
    if (strcasecmp(method, "PUT") != 0 && strcasecmp(method, "DELETE") != 0) {
        file = fopen(full_path, "r");
    }

    // 具体的对每个请求方法的处理策略
    if (strcasecmp(method, "GET") == 0) {
        if (file) {
            content_length = fread(content, 1, sizeof(content), file);
            //格式输出到响应头
            sprintf(response, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", content_length);
            //发送响应头和内容
            write(client_socket, response, strlen(response));
            write(client_socket, content, content_length);
        } else {
            char *error_message = "404 Not Found";
            sprintf(response, "HTTP/1.0 404 Not Found\r\nContent-Length: %zu\r\n\r\n%s", strlen(error_message), error_message);
            write(client_socket, response, strlen(response));
        }
    } else if (strcasecmp(method, "HEAD") == 0) {
        if (file) {
            //不读取文件计算文件长度的一种做法，搭配fseek和ftell
            fseek(file, 0, SEEK_END);
            content_length = ftell(file);
            sprintf(response, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", content_length);
        } else {
            char *error_message = "404 Not Found";
            sprintf(response, "HTTP/1.0 404 Not Found\r\nContent-Length: %zu\r\n\r\n", strlen(error_message));
        }
        write(client_socket, response, strlen(response));
    } else if (strcasecmp(method, "PUT") == 0) {
        // PUT必然涉及到新建文件，所以要给写权限
        file = fopen(full_path, "w");
        if (file) {
            char *body = strstr(buffer, "\r\n\r\n") + 4; //把字符串开头偏移到文件内容开头，这也就是为什么要有一个空行分割请求头和请求体的意义
            fprintf(file, "%s", body);
            fclose(file);
            sprintf(response, "HTTP/1.0 201 Created\r\n\r\n");
        } else {
            sprintf(response, "HTTP/1.0 500 Internal Server Error\r\n\r\n");
        }
        write(client_socket, response, strlen(response));
    } else if (strcasecmp(method, "DELETE") == 0) {
        if (remove(full_path) == 0) {
            sprintf(response, "HTTP/1.0 200 OK\r\n\r\n");
        } else {
            sprintf(response, "HTTP/1.0 404 Not Found\r\n\r\n");
        }
        write(client_socket, response, strlen(response));
    } else {
        //没实现的请求方法就返回没实现的状态码501
        sprintf(response, "HTTP/1.0 501 Not Implemented\r\n\r\n");
        write(client_socket, response, strlen(response));
    }

    if (file) {
        fclose(file);
    }
}
