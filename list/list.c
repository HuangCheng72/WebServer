//
// Created by huangcheng on 2024/5/6.
//

#include "list.h"

// 初始化链表头部
void init_list_node(LIST_NODE *node) {
    node->next = node;
    node->prev = node;
}

// 添加节点到链表
void __list_add(LIST_NODE *new,
                              LIST_NODE *prev,
                              LIST_NODE *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

// 在头部之后添加节点
void list_add(LIST_NODE *new, LIST_NODE *head) {
    __list_add(new, head, head->next);
}

// 在尾部之前添加节点
void list_add_tail(LIST_NODE *new, LIST_NODE *head) {
    __list_add(new, head->prev, head);
}

// 从链表中删除节点
void __list_del(LIST_NODE * prev, LIST_NODE * next) {
    next->prev = prev;
    prev->next = next;
}

// 删除节点
void list_del(LIST_NODE *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

// 判断链表是否为空
int list_empty(const LIST_NODE *head) {
    return head->next == head;
}