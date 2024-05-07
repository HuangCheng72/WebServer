//
// Created by huangcheng on 2024/5/6.
//

#include "hashtable.h"

// 动态创建并初始化哈希表
HashTable *createHashTable(int size) {
    HashTable *hashTable = malloc(sizeof(HashTable));
    if (hashTable == NULL) {
        return NULL;
    }

    hashTable->table = malloc(sizeof(LIST_NODE) * size);
    if (hashTable->table == NULL) {
        free(hashTable);
        return NULL;
    }

    hashTable->size = size;
    for (int i = 0; i < size; i++) {
        init_list_node(&hashTable->table[i]);
    }

    // 哈希表拥有的互斥锁初始化
//    pthread_mutex_init(&(hashTable->mutex), NULL);

    return hashTable;
}

// 最简单的哈希函数
int hashFunction(int key, int size) {
    return key % size;
}

// 插入键值对
void hashtable_insert(HashTable *hashTable, int key, void *value) {
    if (hashTable == NULL) return;
//    pthread_mutex_lock(&(hashTable->mutex));    // 加锁
    int index = hashFunction(key, hashTable->size);
    HashNode *newNode = malloc(sizeof(HashNode));
    if (newNode == NULL) {
        return;
    }
    newNode->key = key;
    newNode->value = value;
    init_list_node(&newNode->list);
    list_add_tail(&newNode->list, &hashTable->table[index]);
//    pthread_mutex_unlock(&(hashTable->mutex));  // 解锁
}

// 查找键对应的值
void* hashtable_find(HashTable *hashTable, int key) {
    if (hashTable == NULL) return NULL;
    int index = hashFunction(key, hashTable->size);
    LIST_NODE *pos;
    list_for_each(pos, &hashTable->table[index]) {
        HashNode *node = list_entry(pos, HashNode, list);
        if (node->key == key) {
            return node->value;
        }
    }
    return NULL;  // 如果找不到，返回NULL
}

// 删除键值对
void hashtable_delete(HashTable *hashTable, int key) {
    if (hashTable == NULL) return;
//    pthread_mutex_lock(&(hashTable->mutex));    // 加锁
    int index = hashFunction(key, hashTable->size);
    LIST_NODE *pos, *n;
    list_for_each_safe(pos, n, &hashTable->table[index]) {
        HashNode *node = list_entry(pos, HashNode, list);
        if (node->key == key) {
            list_del(pos);
            free(node);
            break;
        }
    }
//    pthread_mutex_unlock(&(hashTable->mutex));  // 解锁
}

// 判断键是否存在
int hashtable_contains(HashTable *hashTable, int key) {
    if (hashTable == NULL) return 0;
    return hashtable_find(hashTable, key) != NULL;
}

// 清理哈希表
void freeHashTable(HashTable *hashTable) {
    if (hashTable == NULL) return;
    LIST_NODE *pos, *n;
    for (int i = 0; i < hashTable->size; i++) {
        list_for_each_safe(pos, n, &hashTable->table[i]) {
            HashNode *node = list_entry(pos, HashNode, list);
            list_del(pos);
            free(node);
        }
    }
    free(hashTable->table);
    free(hashTable);
}
