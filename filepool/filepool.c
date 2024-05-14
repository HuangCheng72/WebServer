//
// Created by huangcheng on 2024/5/13.
//

#include "filepool.h"

// 程序是否继续运行标志
extern int keep_running;

//交换两个元素，该函数不对外暴露
void exchange_file(FILEPOOL *pFILEPOOL, int i,int j){
    if(i == j){
        return;
    }
    //交换指针元素
    FILEINFO * temp = pFILEPOOL->pFILEINFO[i];
    pFILEPOOL->pFILEINFO[i] = pFILEPOOL->pFILEINFO[j];
    pFILEPOOL->pFILEINFO[j] = temp;
    //更新下标
    pFILEPOOL->pFILEINFO[i]->index = i;
    pFILEPOOL->pFILEINFO[j]->index = j;
}
//元素上浮，返回最终的位置，该函数不对外暴露
void swim_file(FILEPOOL *pFILEPOOL, int i){
    //如果下标为i处指针指向的TCPINFO剩余超时时间小于下标为i/2指向的TCPINFO剩余超时时间，就交换到前面去
    while(pFILEPOOL->pFILEINFO[i]->timeout < pFILEPOOL->pFILEINFO[i/2]->timeout){//因为下标为0处是哨兵，交换到下标为1就是堆顶了，不会再上浮
        exchange_file(pFILEPOOL, i, i/2);
        i /= 2;
    }
}
//元素下沉，该函数不对外暴露
void sink_file(FILEPOOL *pFILEPOOL, int i){
    //若是i处指针指向的TCPINFO剩余超时时间大于i*2或者i*2+1处指针指向的TCPINFO剩余超时时间，就交换到较小的那个位置
    //注意堆的规模是count
    while(2 * i <= pFILEPOOL->count){
        //比较i*2和i*2+1哪个更小
        int j = 2 * i;
        if(j < pFILEPOOL->count && pFILEPOOL->pFILEINFO[j+1]->timeout < pFILEPOOL->pFILEINFO[j]->timeout){
            j++;
        }
        if(pFILEPOOL->pFILEINFO[i]->timeout < pFILEPOOL->pFILEINFO[j]->timeout){
            //i处指针指向的TCPINFO剩余超时时间已经比两个当中最小的那一个都要小了，说明符合小顶堆性质，退出
            return;
        }
        //不符合小顶堆性质，就交换
        exchange_file(pFILEPOOL, i, j);
        //更新j的值
        i = j;
    }
}

FILEPOOL *CreateFilePool(int timeout){

    FILEPOOL *pFILEPOOL = (FILEPOOL *)malloc(sizeof(FILEPOOL));
    if (!pFILEPOOL) {
        perror("Failed to allocate memory for FILEPOOL");
        return NULL;
    }

    pFILEPOOL->count = 0;
    pFILEPOOL->size = 128;    // 初始默认为128，后续支持扩容，所以要用这个记录容量
    pFILEPOOL->timeout = timeout;

    pFILEPOOL->pFILEINFO = (FILEPOOL **)malloc(sizeof(FILEPOOL*) * pFILEPOOL->size); //动态数组申请方式
    if (!pFILEPOOL->pFILEINFO) {
        perror("Failed to allocate memory for FILEINFO");
        free(pFILEPOOL);
        return NULL;
    }

    //初始化，指针数组置空
    for(int i = 0; i < pFILEPOOL->size; i++) {
        pFILEPOOL->pFILEINFO[i] = NULL;
    }

    //这个结点是同时作为头结点和哨兵结点用的
    FILEINFO *DummyNode = (FILEINFO *)malloc(sizeof(FILEINFO));
    DummyNode->fd = -1;                     // 这表示它没有管理一个有效的文件
    DummyNode->filename = NULL;             // 同上
    DummyNode->timeout = 0;                 // 没有比它更小的值了，才能用作哨兵，挡住一切比它大的
    DummyNode->index = 0;                   // 哨兵结点专属位置
    init_list_node(&(DummyNode->node));     // 初始化这个结点，让它可以发挥头结点的用途


    // 设置链表头结点，并且在指针数组里面也加入哨兵结点的位置
    pFILEPOOL->head = DummyNode;
    pFILEPOOL->pFILEINFO[0] = DummyNode;


    // 建立哈希表，大小为701是个质数，使用起来效果好一点
    pFILEPOOL->pHashTable_filename_pFILEINFO = CreateHashTableStrKey(701);

    // 连接池的线程互斥锁
    pthread_mutex_init(&pFILEPOOL->lock, NULL);

    // 返回设置好的头结点
    return pFILEPOOL;
}

// 扩容方法，不对外暴露，缩小容量暂时不考虑
void FILEPOOLResize(FILEPOOL *pFILEPOOL) {
    if(pFILEPOOL->pFILEINFO == NULL) {
        return;
    }
    //扩容策略就是最简单的两倍扩容
    int newsize = pFILEPOOL->size * 2;
    FILEINFO ** newPtrArray = (FILEPOOL **)malloc(sizeof(FILEPOOL*) * newsize);   //动态数组申请方式
    //初始化和拷贝信息
    for(int i = 0; i < newsize; i++) {
        if(i < pFILEPOOL->size) {
            newPtrArray[i] = pFILEPOOL->pFILEINFO[i];
        } else {
            newPtrArray[i] = NULL;
        }
    }
    //信息转移成功，销毁旧数组，设置新数组
    free(pFILEPOOL->pFILEINFO);
    pFILEPOOL->pFILEINFO = newPtrArray;
    pFILEPOOL->size = newsize;
}


FILEINFO *CreateFileInfo(const char *filename) {
    // 首先尝试打开，拿到完整读写权限
    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        // 打开失败不用看了
        return NULL;
    }

    // 创建结构体
    FILEINFO *pFILEINFO = (FILEINFO *) malloc(sizeof(FILEINFO));
    // 处理信息
    pFILEINFO->filename = strdup(filename);
    pFILEINFO->fd = fd;

    // stat是Linux内核里面声明的一个结构体，Linux的文件的所有信息就存储在这样的一个结构体里面
    // 用系统调用fstat，把文件的信息保存到我们的对应的结构体变量中
    fstat(fd, &pFILEINFO->filestat);

    // 管理信息
    pFILEINFO->index = -1;
    pFILEINFO->timeout = -1;                                    // 设置为-1方便后续判断
    init_list_node(&(pFILEINFO->node));                         // 初始化结点
    pthread_rwlock_init(&pFILEINFO->rwlock, NULL);         // 初始化读写锁
    pFILEINFO->write_lock_active = 0;
    pFILEINFO->read_lock_count = 0;

    return pFILEINFO;

}

void FreeFileInfo(FILEINFO *pFILEINFO) {
    // 完全回收资源
    list_del(&(pFILEINFO->node));
    free(pFILEINFO->filename);
    pthread_rwlock_destroy(&pFILEINFO->rwlock);
    // 回收整个信息结构体
    free(pFILEINFO);
}

int AddFileToFilePool(FILEPOOL *pFILEPOOL, FILEINFO *pFILEINFO) {
    if (pFILEPOOL == NULL || pFILEPOOL->pHashTable_filename_pFILEINFO == NULL) {
        return 0;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pFILEPOOL->lock)) {
        // 加锁失败，文件池出问题了
        return 0;
    }

    if (!HashTableStrKey_Contains(pFILEPOOL->pHashTable_filename_pFILEINFO, pFILEINFO->filename)) {
        // 设置管理信息
        pFILEINFO->index = ++pFILEPOOL->count;                      // 管理文件的数量增多，每次都先插入到堆底位置
        pFILEINFO->timeout = -1;

        // 只需要设置管理信息就行了，处理信息是不变的

        // 放置并管理
        if (pFILEPOOL->count == pFILEPOOL->size) {
            // 满了，扩容
            FILEPOOLResize(pFILEPOOL);
        }

        pFILEPOOL->pFILEINFO[pFILEPOOL->count] = pFILEINFO;         // 插入到堆底位置
        list_add(&(pFILEPOOL->head->node), &(pFILEINFO->node));     // 存入存储结构

        HashTableStrKey_Insert(pFILEPOOL->pHashTable_filename_pFILEINFO, pFILEINFO->filename, pFILEINFO);  // 建立文件名和文件信息指针之间的关系

    }

    pFILEINFO->timeout = pFILEPOOL->timeout;                    // 无论如何重设超时时间
    // 因为重设超时之后，肯定是最大的，必须放到堆底
    exchange_file(pFILEPOOL, pFILEINFO->index, pFILEPOOL->count);
    // 只需要上浮调整即可
    swim_file(pFILEPOOL, pFILEPOOL->count);                     // 堆底元素上浮，完成堆的性质维护

    pthread_mutex_unlock(&pFILEPOOL->lock);   // 解锁

    return 1;
}

int RemoveFileFromFilePool(FILEPOOL *pFILEPOOL, FILEINFO *pFILEINFO) {
    // 首先，先判断这一元素是不是这个连接池管理的
    // 如果连接池当前管理的堆规模是0，说明这个不属于这个连接池管理
    // 如果TCP结点存储的下标比连接池的总规模还大，说明也不属于这个连接池管理
    // 后面三个条件说明已经被移除了
    if(pFILEPOOL->count == 0 || pFILEPOOL->size <= pFILEINFO->index || pFILEINFO->timeout == -1 || pFILEINFO->index == -1 || pFILEINFO->fd == -1){
        return -1;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pFILEPOOL->lock)) {
        // 加锁失败，连接池出问题了
        return -1;
    }

    // 首先把这个指针交换到堆末尾，然后再缩小堆的规模
    // 缩小堆的规模，就意味着不再受到堆的管理，不需要真的执行删除操作
    int index = pFILEINFO->index;
    exchange_file(pFILEPOOL, index, pFILEPOOL->count);
    --pFILEPOOL->count;
    sink_file(pFILEPOOL, index);              // 然后调整顺序（因为是从堆底交换过来的，所以要下沉），完成堆的性质维护

    list_del(&(pFILEINFO->node));           // 从存储链表中删除这个文件信息
    HashTableStrKey_Delete(pFILEPOOL->pHashTable_filename_pFILEINFO, pFILEINFO->filename);  // 删除文件名和文件信息结构体指针之间的关系

    // 把这个文件信息从连接池中取出，还是要处理的，因此要保留处理信息，管理信息可以删掉，防止出错
    pFILEINFO->timeout = -1;             // 设置为一个不可能的值，防止出错
    pFILEINFO->index = -1;               // 设置为一个不可能的值，防止出错
    int fd = pFILEINFO->fd;
    pFILEINFO->fd = -1;       // 设置为一个不可能的值，防止出错
    init_list_node(&(pFILEINFO->node));   // 让这个结点成为一个自环，也就是初始化状态

    pthread_mutex_unlock(&pFILEPOOL->lock);   // 解锁

    // 返回文件描述符
    return fd;
}

void FreeFilePool(FILEPOOL *pFILEPOOL) {
    if (!pFILEPOOL) return;

    // 直接关闭所有的TCP连接，然后从连接池中删除所有连接
    for (int i = 1; i < pFILEPOOL->count; i++) {
        FILEINFO *pFILEINFO = pFILEPOOL->pFILEINFO[i];
        // 关闭连接并回收信息结构体
        if (pFILEINFO && pFILEINFO->fd > -1) {
            close(pFILEINFO->fd);
            pFILEINFO->fd = -1;   //防止再被错误关闭
        }
        // 回收信息结构体
        RemoveFileFromFilePool(pFILEPOOL, pFILEINFO);
        FreeFileInfo(pFILEINFO);
    }
    // 回收互斥锁
    pthread_mutex_destroy(&pFILEPOOL->lock);
    // 回收指针数组和哈希表
    FreeHashTableStrKey(pFILEPOOL->pHashTable_filename_pFILEINFO);
    free(pFILEPOOL->pFILEINFO);
    // 回收头结点
    free(pFILEPOOL->head);
    // 回收整个连接池
    free(pFILEPOOL);
}

void PrintFilePoolStatus(FILEPOOL *pFILEPOOL) {
    if(!pFILEPOOL) return;
    pthread_mutex_lock(&pFILEPOOL->lock);
    printf("文件池中现有文件数量为: %d\n", pFILEPOOL->count);
    pthread_mutex_unlock(&pFILEPOOL->lock);
}

void* filePoolThread(void* arg) {
    FILEPOOL * pFILEPOOL = (FILEPOOL *)arg;

    // 注册当前线程到计时管理线程
    TimerThreadControl *timer_ctrl = register_timer_thread(pthread_self());
    // 存在依赖关系，不得不重试
    while (!timer_ctrl) {
        timer_ctrl = register_timer_thread(pthread_self());
        if (!timer_ctrl) {
            sleep(1);  // 睡眠1秒后重试
        }
    }

    while (1 && keep_running) {

        pthread_mutex_lock(timer_ctrl->mutex);
        // 设置超时时间为当前时间，即不等待
        int wait_result = pthread_cond_wait(timer_ctrl->cond, timer_ctrl->mutex);
        pthread_mutex_unlock(timer_ctrl->mutex);

        if(wait_result == 0) {
            // 收到信号，已经过了一秒钟了
            // 堆顶是下标为1的位置
            for(int i = 1; i < pFILEPOOL->count + 1; i++) {
                // 正在使用中一律跳过
                if(pFILEPOOL->pFILEINFO[i]->read_lock_count > 0 || pFILEPOOL->pFILEINFO[i]->write_lock_active > 0) {
                    continue;
                }
                --pFILEPOOL->pFILEINFO[i]->timeout;
            }
            // 检查有没有剩余超时时间已经为0的（因为是最小堆，如果堆顶都没超时，其他的更不用说），有的话就全部关掉
            while(pFILEPOOL->count > 0 && pFILEPOOL->pFILEINFO[1]->timeout <= 0) {
                // 从连接池中删除并关闭其连接
                FILEINFO *pFILEINFOtop = pFILEPOOL->pFILEINFO[1];
                int fd = RemoveFileFromFilePool(pFILEPOOL, pFILEINFOtop);
                FreeFileInfo(pFILEINFOtop);      // 回收连接信息
                if(fd > -1) {
                    close(fd);
                }
            }
        }
    }
    //释放整个文件池
    FreeFilePool(pFILEPOOL);
    pthread_exit(NULL);
}


// 从文件池中获取文件
FileResult *GetFileFromFilePool(FILEPOOL *pFILEPOOL, const char *filename, int isWrite) {
    if (pFILEPOOL == NULL) {
        return NULL;
    }

    // 锁定文件池互斥锁
    pthread_mutex_lock(&pFILEPOOL->lock);

    FILEINFO *pFILEINFO = (FILEINFO *)HashTableStrKey_Find(pFILEPOOL->pHashTable_filename_pFILEINFO, filename);

    pthread_mutex_unlock(&pFILEPOOL->lock);

    if (!pFILEINFO) {
        // 如果没有找到文件信息，则创建新的文件信息
        pFILEINFO = CreateFileInfo(filename);
        if(!pFILEINFO){
            return NULL;
        }
    }
    FileResult *result = (FileResult *)malloc(sizeof(FileResult));

    // 重新插入
    AddFileToFilePool(pFILEPOOL, pFILEINFO);

    // 根据请求类型加锁
    if (isWrite) {
        // 请求写权限就加写锁
        pthread_rwlock_wrlock(&pFILEINFO->rwlock);
        pFILEINFO->write_lock_active = 1;   // 写锁状态更新
    } else {
        // 请求读权限就加读锁
        pthread_rwlock_rdlock(&pFILEINFO->rwlock);
        pFILEINFO->read_lock_count++;       // 读锁计数更新
    }
    result->filename = strdup(filename);
    result->fd = pFILEINFO->fd;
    memcpy(&result->filestat, &pFILEINFO->filestat, sizeof(struct stat));

    // 返回前记得将文件偏移量设置为0
    lseek(result->fd, 0, SEEK_SET);

    return result;
}
// 归还文件控制权给文件池
void ReturnFileToFilePool(FILEPOOL *pFILEPOOL, FileResult *result, int isWrite) {
    if (pFILEPOOL == NULL) return;

    // 锁定文件池互斥锁
    pthread_mutex_lock(&pFILEPOOL->lock);

    FILEINFO *pFILEINFO = (FILEINFO *)HashTableStrKey_Find(pFILEPOOL->pHashTable_filename_pFILEINFO, result->filename);
    if (pFILEINFO) {
        // 根据请求类型加锁
        if (isWrite) {
            // 请求写权限就解写锁
            pthread_rwlock_unlock(&pFILEINFO->rwlock);
            pFILEINFO->write_lock_active = 0;   // 写锁状态更新
            // 保存并重新打开
            close(pFILEINFO->fd);
            pFILEINFO->fd = -1;
            pFILEINFO->fd = open(pFILEINFO->filename, O_RDWR);
            if(pFILEINFO->fd < 0) {
                // 这种情况说明出事了，文件控制权不在文件池这里了，应该立即抛弃之
                RemoveFileFromFilePool(pFILEPOOL, pFILEINFO);
                FreeFileInfo(pFILEINFO);
            } else {
                // 重新读取文件信息
                fstat(pFILEINFO->fd, &pFILEINFO->filestat);
            }
        } else {
            // 请求读权限就解读锁
            pthread_rwlock_unlock(&pFILEINFO->rwlock);
            pFILEINFO->read_lock_count--;       // 读锁计数更新
        }
    }
    //销毁result
    free(result->filename);
    free(result);

    pthread_mutex_unlock(&pFILEPOOL->lock);

}

