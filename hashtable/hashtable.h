//
// Created by huangcheng on 2024/5/6.
//

#ifndef WEBSERVER_HASHTABLE_H
#define WEBSERVER_HASHTABLE_H

#include "../list/list.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

// 这里是一个哈希表的实现
// 目前为止，一个实例里面只有一个哈希表，一个实例又只归一个线程管，那就没必要线程安全了，锁用线程锁

// 以下是整数作为键的实现版本
typedef struct __HashNode_Int {
    int key;
    void *value;
    LIST_NODE list;
} HashNodeInt;

typedef struct __HashTable_Int {
    LIST_NODE *table;
    int size;
    pthread_mutex_t mutex;
} HashTableInt;

/**
 * 创建并初始化哈希表
 * @param size 哈希表大小
 * @return 指向哈希表的指针
 */
HashTableInt *CreateHashTableIntKey(int size);

/**
 * 插入键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @param value 值
 */
void HashTableIntKey_Insert(HashTableInt *hashTable, int key, void *value);

/**
 * 查找键对应的值
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 值（如果查找不到返回空指针NULL）
 */
void *HashTableIntKey_Find(HashTableInt *hashTable, int key);

/**
 * 从哈希表中删除键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 */
void HashTableIntKey_Delete(HashTableInt *hashTable, int key);

/**
 * 判断键是否存在于哈希表中
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 存在返回1，不存在返回0
 */
int HashTableIntKey_Contains(HashTableInt *hashTable, int key);

/**
 * 完全清理一个哈希表
 * @param hashTable 指向操作的哈希表的指针
 */
void FreeHashTableIntKey(HashTableInt *hashTable);


// 以下是字符串作为键的实现版本
typedef struct __HashNode_Str {
    char *key;
    void *value;
    LIST_NODE list;
} HashNodeStr;

typedef struct __HashTable_Str {
    LIST_NODE *table;
    int size;
    pthread_mutex_t mutex;
} HashTableStr;

/**
 * 创建并初始化哈希表
 * @param size 哈希表大小
 * @return 指向哈希表的指针
 */
HashTableStr *CreateHashTableStrKey(int size);

/**
 * 插入键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @param value 值
 */
void HashTableStrKey_Insert(HashTableStr *hashTable,const char *key, void *value);

/**
 * 查找键对应的值
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 值（如果查找不到返回空指针NULL）
 */
void *HashTableStrKey_Find(HashTableStr *hashTable,const char *key);

/**
 * 从哈希表中删除键值对
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 */
void HashTableStrKey_Delete(HashTableStr *hashTable,const char *key);

/**
 * 判断键是否存在于哈希表中
 * @param hashTable 指向操作的哈希表的指针
 * @param key 键
 * @return 存在返回1，不存在返回0
 */
int HashTableStrKey_Contains(HashTableStr *hashTable,const char *key);

/**
 * 完全清理一个哈希表
 * @param hashTable 指向操作的哈希表的指针
 */
void FreeHashTableStrKey(HashTableStr *hashTable);

#endif //WEBSERVER_HASHTABLE_H
