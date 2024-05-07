//
// Created by huangcheng on 2024/5/5.
//

#ifndef WEBSERVER_LIST_H
#define WEBSERVER_LIST_H

#include <stdlib.h>
#include <assert.h>

// 这里提供了一个内核双向链表的实现

// 链表节点结构
typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} LIST_NODE;

// 创建并初始化节点
#define LIST_NODE_INIT(name) { &(name), &(name) }

// 定义并初始化链表
#define LIST_HEAD_INIT(name) { &(name), &(name) }

// 创建并初始化一个链表头部
#define LIST_HEAD(name) \
    LIST_NODE name = LIST_HEAD_INIT(name)

/**
 * 初始化链表结点，返回一个已经成为自环的结点，可以作为头结点或者进行任何操作。
 * @param list 指向链表结点的指针
 */
void init_list_node(LIST_NODE *node);

/**
 * 头插法插入结点
 * @param new 指向新结点的指针
 * @param head 指向头结点的指针
 */
void list_add(LIST_NODE *new, LIST_NODE *head);

/**
 * 尾插法插入结点
 * @param new 指向新结点的指针
 * @param head 指向头结点的指针
 */
void list_add_tail(LIST_NODE *new, LIST_NODE *head);

/**
 * 删除结点
 * @param entry 指向要删除结点的指针
 */
void list_del(LIST_NODE *entry);

/**
 * 判断链表是否为空
 * @param head 指向头结点的指针
 * @return 为空返回1，不为空返回0
 */
int list_empty(const LIST_NODE *head);

// 获取到结构体的首地址
#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

// 遍历链表
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

// 安全的遍历链表
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
        pos = n, n = pos->next)


#endif //WEBSERVER_LIST_H
