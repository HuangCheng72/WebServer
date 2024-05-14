//
// Created by huangcheng on 2024/5/13.
//

#ifndef WEBSERVER_FILEPOOL_H
#define WEBSERVER_FILEPOOL_H

#include "../hashtable/hashtable.h"
#include "../timer/timerthread.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

// 这是一个文件池的实现
// 对这个文件池的操作必须是线程安全的

typedef struct file_result {
    char *filename;
    int fd;
    struct stat filestat;
} FileResult;

typedef struct fileinfo {
    // 管理信息
    int timeout;                // 超时阈值
    int index;                  // 在等待数组中的下标，方便堆操作
    //内核链表嵌入结构体中，用链表来串联起每个结点，用于存储在内存中
    LIST_NODE node;
    pthread_rwlock_t rwlock;    // 读写锁
    // 锁的使用情况统计
    int read_lock_count;        // 当前持有读锁的线程数
    int write_lock_active;      // 写锁是否被激活（0为无，1为有）


    // 处理信息
    int fd;                     // 存储的文件描述符
    char *filename;             // 存完整文件名避免真的哈希冲突的问题
    // stat是Linux内核里面声明的一个结构体，Linux的文件的所有信息就存储在这样的一个结构体里面
    struct stat filestat;            // Linux系统中存储的完整文件信息

} FILEINFO;

typedef struct filepool {
    // count为正在被管理的文件数，size为数组大小
    volatile int count;
    // count变动性太强，要加上volatile，不得从缓存读取，size变动性不强
    int size;
    // 设置这个文件池管理的文件的超时时间
    int timeout;

    // 指针数组，在指针数组上建立数据结构，比在结构体数组数组上建立数据结构容易，因为指针更容易交换
    // 运用小顶堆思想改写（下标从1开始比较好计算），可以在pFILEINFO[0]处放置哨兵
    // 以timeout为排序键值
    FILEINFO **pFILEINFO;

    // 实际存储TCPINFO的存储结构是链表
    FILEINFO *head;

    // 文件名和指向FILEINFO的指针之间的哈希表，文件描述符为键，指针为值
    // 使用原因是不知道文件名对应的指针，所以用哈希表来方便查找，不然就要遍历了
    HashTableStr *pHashTable_filename_pFILEINFO;

    pthread_mutex_t lock;     // 线程互斥锁

} FILEPOOL;

/**
 * 创建一个已经初始化的文件池
 * @param timeout 文件池实例的超时时间
 * @return 文件池指针
 */
FILEPOOL *CreateFilePool(int timeout);

/**
 * 输出文件池的状态
 * @param pFILEPOOL 指向文件池的指针
 */
void PrintFilePoolStatus(FILEPOOL *pFILEPOOL);

/**
 * 文件池管理线程函数
 * @param arg 传入管理的文件池指针
 * @return
 */
void* filePoolThread(void* arg);

/**
 * 从文件池中获取文件
 * @param pFILEPOOL 指向文件池的指针
 * @param filename 文件名
 * @param isWrite 是否属于写入操作，是就传入1，不是就传入0
 * @param filestat 需要文件信息的话，在这里给出一个指针，文件信息将写出到这个指针
 * @return 返回一个文件结果结构体指针
 */
FileResult *GetFileFromFilePool(FILEPOOL *pFILEPOOL,const char *filename, int isWrite);

/**
 * 归还文件控制权给文件池
 * @param pFILEPOOL 指向文件池的指针
 * @param result GetFileFromFilePool返回的文件结果结构体指针
 * @param isWrite 是否属于写入操作，是就传入1，不是就传入0
 */
void ReturnFileToFilePool(FILEPOOL *pFILEPOOL, FileResult *result, int isWrite);

#endif //WEBSERVER_FILEPOOL_H
