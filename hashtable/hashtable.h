//
// Created by huangcheng on 2024/5/6.
//

#ifndef WEBSERVER_HASHTABLE_H
#define WEBSERVER_HASHTABLE_H

#include "../list/list.h"
#include <stdlib.h>
#include <pthread.h>

// 这里是一个线程安全哈希表的实现

typedef struct __HashNode {
    int key;
    void *value;
    LIST_NODE list;
} HashNode;

typedef struct __HashTable {
    LIST_NODE *table;
    int size;
//    pthread_mutex_t mutex;
} HashTable;

/**
 * 创建并初始化哈希表
 * @param size 哈希表大小
 * @return 指向哈希表的指针
 */
HashTable *createHashTable(int size);

/**
 * 插入键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @param value 值
 */
void hashtable_insert(HashTable *hashTable, int key, void *value);

/**
 * 查找键对应的值
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 值（如果查找不到返回空指针NULL）
 */
void *hashtable_find(HashTable *hashTable, int key);

/**
 * 从哈希表中删除键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 */
void hashtable_delete(HashTable *hashTable, int key);

/**
 * 判断键是否存在于哈希表中
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 存在返回1，不存在返回0
 */
int hashtable_contains(HashTable *hashTable, int key);

/**
 * 完全清理一个哈希表
 * @param hashTable 指向操作的哈希表的指针
 */
void freeHashTable(HashTable *hashTable);

#endif //WEBSERVER_HASHTABLE_H
