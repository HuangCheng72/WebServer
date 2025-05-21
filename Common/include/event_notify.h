//
// Created by huangcheng on 2025/5/21.
//

/**
 * 提供Linux内核支持的轻量级事件通知机制eventfd的操作函数
 */

#ifndef WEBSERVER_EVENT_NOTIFY_H
#define WEBSERVER_EVENT_NOTIFY_H

#include <unistd.h>
#include <stdint.h>

/**
 * 创建一个 eventfd。
 * @return 成功返回 eventfd 文件描述符，失败返回 -1
 */
int eventfd_create();

/**
 * 销毁一个 eventfd。
 * @param fd 要销毁的 eventfd 文件描述符
 */
void eventfd_destroy(int fd);

/**
 * 发送一个通知信号。
 * @param fd eventfd 的文件描述符
 * @return 成功返回 0，失败返回 -1
 */
int eventfd_notify(int fd);

/**
 * 等待并消费一个通知信号。
 * @param fd eventfd 的文件描述符
 * @return 成功返回通知计数（通常为 1），失败返回 -1
 */
int eventfd_wait(int fd);

#endif //WEBSERVER_EVENT_NOTIFY_H
