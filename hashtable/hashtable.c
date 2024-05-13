//
// Created by huangcheng on 2024/5/6.
//

#include "hashtable.h"

// 动态创建并初始化哈希表
HashTableInt *CreateHashTableIntKey(int size) {
    HashTableInt *hashtable = malloc(sizeof(HashTableInt));
    if (hashtable == NULL) {
        return NULL;
    }

    hashtable->table = malloc(sizeof(LIST_NODE) * size);
    if (hashtable->table == NULL) {
        free(hashtable);
        return NULL;
    }

    hashtable->size = size;
    for (int i = 0; i < size; i++) {
        init_list_node(&hashtable->table[i]);
    }

    return hashtable;
}

// 最简单的哈希函数
int hashFunctionInt(int key, int size) {
    return key % size;
}

// 插入键值对
void HashTableIntKey_Insert(HashTableInt *hashTable, int key, void *value) {
    if (hashTable == NULL) return;
    int index = hashFunctionInt(key, hashTable->size);
    HashNodeInt *newNode = malloc(sizeof(HashNodeInt));
    if (newNode == NULL) {
        return;
    }
    newNode->key = key;
    newNode->value = value;
    init_list_node(&newNode->list);
    list_add_tail(&newNode->list, &hashTable->table[index]);
}

// 查找键对应的值
void* HashTableIntKey_Find(HashTableInt *hashTable, int key) {
    if (hashTable == NULL) return NULL;
    int index = hashFunctionInt(key, hashTable->size);
    LIST_NODE *pos;
    list_for_each(pos, &hashTable->table[index]) {
        HashNodeInt *node = list_entry(pos, HashNodeInt, list);
        if (node->key == key) {
            return node->value;
        }
    }
    return NULL;  // 如果找不到，返回NULL
}

// 删除键值对
void HashTableIntKey_Delete(HashTableInt *hashTable, int key) {
    if (hashTable == NULL) return;
    int index = hashFunctionInt(key, hashTable->size);
    LIST_NODE *pos, *n;
    list_for_each_safe(pos, n, &hashTable->table[index]) {
        HashNodeInt *node = list_entry(pos, HashNodeInt, list);
        if (node->key == key) {
            list_del(pos);
            free(node);
            break;
        }
    }
}

// 判断键是否存在
int HashTableIntKey_Contains(HashTableInt *hashTable, int key) {
    if (hashTable == NULL) return 0;
    return HashTableIntKey_Find(hashTable, key) != NULL;
}

// 清理哈希表
void FreeHashTableIntKey(HashTableInt *hashTable) {
    if (hashTable == NULL) return;
    LIST_NODE *pos, *n;
    for (int i = 0; i < hashTable->size; i++) {
        list_for_each_safe(pos, n, &hashTable->table[i]) {
            HashNodeInt *node = list_entry(pos, HashNodeInt, list);
            list_del(pos);
            free(node);
        }
    }
    free(hashTable->table);
    free(hashTable);
}


// 动态创建并初始化哈希表
HashTableStr *CreateHashTableStrKey(int size) {
    HashTableStr *hashtable = malloc(sizeof(HashTableStr));
    if (hashtable == NULL) {
        return NULL;
    }

    hashtable->table = malloc(sizeof(LIST_NODE) * size);
    if (hashtable->table == NULL) {
        free(hashtable);
        return NULL;
    }

    hashtable->size = size;
    for (int i = 0; i < size; i++) {
        init_list_node(&hashtable->table[i]);
    }

    return hashtable;
}

// 最常用的字符串哈希函数
// djb2 算法
unsigned int hashFunctionStr(const char *key, int size) {
    unsigned long hash = 5381;
    int c;

    while ((c = *key++)) {
        // hash = hash * 33 + c
        hash = ((hash << 5) + hash) + c;
    }

    return hash % size;
}

// 插入键值对
void HashTableStrKey_Insert(HashTableStr *hashTable,const char *key, void *value) {
    if (hashTable == NULL) return;
    unsigned int index = hashFunctionStr(key, hashTable->size);
    HashNodeStr *newNode = malloc(sizeof(HashNodeStr));
    if (newNode == NULL) {
        return;
    }
    newNode->key = strdup(key);  // 复制字符串键
    newNode->value = value;
    init_list_node(&newNode->list);
    list_add_tail(&newNode->list, &hashTable->table[index]);
}

// 查找键对应的值
void* HashTableStrKey_Find(HashTableStr *hashTable,const char *key) {
    if (hashTable == NULL) return NULL;
    unsigned int index = hashFunctionStr(key, hashTable->size);
    LIST_NODE *pos;
    list_for_each(pos, &hashTable->table[index]) {
        HashNodeStr *node = list_entry(pos, HashNodeStr, list);
        if (strcmp(node->key, key) == 0) {  // 使用strcmp进行字符串比较
            return node->value;
        }
    }
    return NULL;  // 如果找不到，返回NULL
}

// 删除键值对
void HashTableStrKey_Delete(HashTableStr *hashTable, const char *key) {
    if (hashTable == NULL) return;
    unsigned int index = hashFunctionStr(key, hashTable->size);
    LIST_NODE *pos, *n;
    list_for_each_safe(pos, n, &hashTable->table[index]) {
        HashNodeStr *node = list_entry(pos, HashNodeStr, list);
        if (strcmp(node->key, key) == 0) {
            list_del(pos);
            free(node);
            break;
        }
    }
}

// 判断键是否存在
int HashTableStrKey_Contains(HashTableStr *hashTable, const char *key) {
    if (hashTable == NULL) return 0;
    return HashTableStrKey_Find(hashTable, key) != NULL;
}

// 清理哈希表
void FreeHashTableStrKey(HashTableStr *hashTable) {
    if (hashTable == NULL) return;
    LIST_NODE *pos, *n;
    for (int i = 0; i < hashTable->size; i++) {
        list_for_each_safe(pos, n, &hashTable->table[i]) {
            HashNodeStr *node = list_entry(pos, HashNodeStr, list);
            list_del(pos);
            free(node->key);
            free(node);
        }
    }
    free(hashTable->table);
    free(hashTable);
}