#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/stat.h>

// 随便一个端口都行，只要没占用、没被防火墙拦截（所以需要在防火墙里面放行）就行。
#define PORT 8080
// 这里我需要解释一下，"/home/hc/web"是我的网站文件夹目录，可以随便修改，或者改成用参数接受不同的路径也可以。
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

    // 拼接完整路径
    char full_path[1024];
    sprintf(full_path, "%s%s", WEB_ROOT, strcmp(path, "/") == 0 ? "/index.html" : path);    //因为/这种默认的情况一般都处理的是index，所以把这种情况默认处理了

    // 具体的对每个请求方法的处理策略
    if (strcasecmp(method, "GET") == 0) {
        // 同样是打开文件，这是Linux的系统调用，权限是只读
        int file = open(full_path, O_RDONLY);

        if (file) {
            // stat是Linux内核里面声明的一个结构体，Linux的文件的所有信息就存储在这样的一个结构体里面
            struct stat stat_buf;
            // 用系统调用，把文件的信息保存到我们刚刚创造的结构体临时变量里面
            fstat(file, &stat_buf);
            //格式输出到响应头
            //stat_buf.st_size就是文件的大小属性，也就是我们要发送的内容长度
            sprintf(response, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\n\r\n", stat_buf.st_size);
            //发送响应头
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
            sprintf(response, "HTTP/1.0 404 Not Found\r\nContent-Length: %zu\r\n\r\n%s", strlen(error_message), error_message);
            //发送响应头
            if (write(client_socket, response, strlen(response)) == -1) {
                perror("response_head");
                close(client_socket);
                return;
            }
        }
    } else {
        //没实现的请求方法就返回没实现的状态码501
        sprintf(response, "HTTP/1.0 501 Not Implemented\r\n\r\n");
        //发送响应头
        if (write(client_socket, response, strlen(response)) == -1) {
            perror("response_head");
            close(client_socket);
            return;
        }
    }
}