//
// Created by huangcheng on 2025/5/21.
//

/**
 * 设计变更总原则：TCP连接流动的方向改为单向流动，方向为：Listener -> Manager -> Workers -> Listener
 * Manager对Worker是一对多
 * Worker对Listener是多对一
 *
 */

/**
 * 数据结构依然使用之前 tcppool 线程中设计的数据结构。
 * 主线程计时，并监控其他线程状态；
 * 监听线程监听TCP连接；
 * 管理线程管理堆中TCP连接，超时关闭。
 * 接收线程负责从管理进程接收TCP连接，加到入堆队列中。
 * 发送线程负责把出堆队列中的TCP连接发送到管理线程负责处理。
 */

#include "list.h"
#include "transfer_fd.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdlib.h>

// TCP连接信息的数据结构，用于整体管理
typedef struct {
    int index;                  // 元素在等待数组中的下标，方便堆操作
    LIST_NODE node;             // 内核链表嵌入结构体中，用链表来串联起每个结点，用于存储在内存中
    int client_socket;          // 代表用户的TCP连接，在Linux中是一个文件描述符
    unsigned int timeout;       // 连接超时阈值
} tcpinfo;

/**
 * 创建TCP连接信息
 * @param client_socket socket的文件描述符
 * @param timeout 超时阈值
 * @return tcp连接信息结构体指针
 */
tcpinfo *Create_tcpinfo(int client_socket, unsigned int timeout) {
    tcpinfo *temp = (tcpinfo*)malloc(sizeof(tcpinfo));
    if(!temp) {
        return NULL;
    }
    temp->index = -1;
    temp->timeout = timeout;
    init_list_node(&(temp->node));
    temp->client_socket = client_socket;
    return temp;
}

/**
 * 销毁TCP连接信息
 * @param pInfo
 */
void Destroy_tcpinfo(tcpinfo *pInfo) {
    // 这里被销毁之前已经删除过了，再次删除就会出现NULL->prev和NULL->next的情况
    if (!pInfo){
        return;
    }
    if(pInfo->client_socket > -1) {
        close(pInfo->client_socket);
    }
    memset(pInfo, 0, sizeof(tcpinfo));
    free(pInfo);
}

/**
 * TCP信息队列结构体
 */
typedef struct {
    LIST_NODE head;
    int size;
    pthread_mutex_t lock;
} tcpinfo_queue;

/**
 * 创建TCP信息队列
 * @return
 */
tcpinfo_queue *Create_tcpinfo_queue() {
    tcpinfo_queue *temp = (tcpinfo_queue*)malloc(sizeof(tcpinfo_queue));
    if(!temp) {
        return NULL;
    }
    init_list_node(&(temp->head));
    temp->size = 0;
    pthread_mutex_init(&temp->lock, NULL);  // 连接队列的互斥锁
    return temp;
}

/**
 * 销毁TCP信息队列
 * 因为使用场景确定为最终资源清理，因此修改为关闭队列中所有的tcp连接
 * @param queue
 */
void Destroy_tcpinfo_queue(tcpinfo_queue *queue) {
    if(!queue || list_empty(&queue->head)) {
        return;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    // 涉及增删链表结点的操作，必须使用 list_for_each_safe
    struct list_node *pos = NULL;
    struct list_node *n = NULL;
    list_for_each_safe(pos, n,&queue->head) {
        list_del(pos);
        Destroy_tcpinfo(list_entry(pos, tcpinfo , node));
        queue->size--;
    }
    init_list_node(&queue->head);
    // 锁在自己手里，解锁，销毁。
    pthread_mutex_unlock(&queue->lock);
    free(queue);
}

/**
 *  TCP信息队列是否为空
 * @param queue 要判断的TCP信息队列
 * @return 为空返回1，不为空返回0
 */
int IsEmpty_tcpinfo_queue(tcpinfo_queue *queue) {
    if(!queue) {
        return 0;   // 无法判断
    }
    return list_empty(&queue->head) && queue->size == 0;
}

/**
 * 把一个TCP连接信息添加到queue
 * @param queue queue的指针
 * @param pInfo 要添加的tcp连接信息
 * @return 成功返回1，失败返回0
 */
int Add_tcpinfo_to_queue(tcpinfo_queue *queue, tcpinfo *pInfo) {
    if(!queue || !pInfo) {
        return 0;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    // 加锁成功可以进行
    // list_del(&pInfo->node);  // 如果已经删除了，那么再次删除就会出现NULL->prev和NULL->next的问题，所以，这里不该删除，应该在remove里面删除
    list_add_tail(&pInfo->node, &queue->head);
    queue->size++;
    pthread_mutex_unlock(&queue->lock);
    return 1;
}

/**
 * 从queue中取出队头元素
 * @param queue queue的指针
 * @return 队头元素的指针
 */
tcpinfo *Remove_tcpinfo_from_queue(tcpinfo_queue *queue) {
    // 获取第一个元素
    if(!queue || list_empty(&queue->head)) {
        return NULL;
    }
    // 既然设计是要求资源操作一定要成功，那就别尝试加锁了，直接阻塞到加锁成功为止
    pthread_mutex_lock(&queue->lock);

    tcpinfo *temp = list_entry(queue->head.next, tcpinfo, node);
    list_del(&temp->node);     // 删除结点，防止影响到其前后的结点
    queue->size--;
    pthread_mutex_unlock(&queue->lock);
    return temp;
}

// 等待进入TCP连接池的TCP信息队列
tcpinfo_queue *accept_queue = NULL;
// 等待离开TCP连接池的TCP信息
tcpinfo_queue *release_queue = NULL;

typedef struct {
    volatile int count;         // 正在被管理的TCP连接数
    int size;                   // 连接池容量大小
    tcpinfo** array_tcpinfo;    // 用作指针数组，在指针数组上建立堆结构，比较方便，因为指针更容易交换
    int* hash_socket_tcpinfo;   // 用数组作为tcpinfo和client_socket之间的哈希表（client_socket的值为下标，值则是指针数组中对应的index）
    tcpinfo* head;              // tcpinfo在内存中的的存储结构是链表
    int epoll_fd;               // 连接池拥有的epoll实例
    pthread_mutex_t mutex;      // 线程互斥锁
} tcppool;

//交换两个元素
void exchange(tcppool *pPOOL, int i, int j){
    if(i == j){
        return;
    }
    // 交换指针元素
    tcpinfo* temp = pPOOL->array_tcpinfo[i];
    pPOOL->array_tcpinfo[i] = pPOOL->array_tcpinfo[j];
    pPOOL->array_tcpinfo[j] = temp;
    // 更新下标
    pPOOL->array_tcpinfo[i]->index = i;
    pPOOL->array_tcpinfo[j]->index = j;
}

//元素上浮，到达应到达的位置
void swim(tcppool *pPOOL, int i){
    // 向上调整到符合小顶堆性质
    while(pPOOL->array_tcpinfo[i]->timeout < pPOOL->array_tcpinfo[i/2]->timeout){   // 因为下标为0处是哨兵，交换到下标为1就是堆顶了，不会再上浮
        exchange(pPOOL, i, i/2);
        i /= 2;
    }
}

// 元素下沉，到达应到达的位置
void sink(tcppool *pPOOL, int i){
    //注意堆的规模是count
    while(2 * i <= pPOOL->count){
        //比较 i*2 和 i*2+1 所指向的元素哪个更小，与更小的那个交换
        int j = 2 * i;
        if(j < pPOOL->count && pPOOL->array_tcpinfo[j+1]->timeout < pPOOL->array_tcpinfo[j]->timeout){
            j++;
        }
        if(pPOOL->array_tcpinfo[i]->timeout < pPOOL->array_tcpinfo[j]->timeout){
            //i处指针指向的tcpinfo超时阈值已经比两个当中最小的那一个都要小了，说明符合小顶堆性质，退出
            return;
        }
        //不符合小顶堆性质，就交换
        exchange(pPOOL, i, j);
        //更新j的值
        i = j;
    }
}

tcppool *CreateTCPPool() {

    int epoll_fd = epoll_create1(0);        // 创建epoll实例，这个没有容量限制
    if (epoll_fd == -1) {
        // 创建失败就退出
        perror("epoll_create1 failed");
        return NULL;
    }

    // 申请各个内存块
    tcppool *pPOOL = (tcppool*)malloc(sizeof(tcppool));
    if (!pPOOL) {
        perror("Failed to allocate memory for tcppool");
        close(epoll_fd);    // 确保即使失败也要关闭epoll文件描述符
        return NULL;
    }
    tcpinfo** temp_array = (tcpinfo**)malloc(sizeof(tcpinfo*) * 4096); // 动态数组申请方式，初始大小设置为4096，主要是防止设置太小，初期就多次扩容
    if (!temp_array) {
        perror("Failed to allocate memory for array_tcpinfo");
        close(epoll_fd);
        free(pPOOL);
        return NULL;
    }
    int* hashtable = (int*)malloc(sizeof(int) * 8192); // 直接用数组作哈希表
    if (!hashtable) {
        perror("Failed to allocate memory for hash_socket_tcpinfo");
        close(epoll_fd);
        free(pPOOL);
        free(temp_array);
        return NULL;
    }
    // 创建哨兵结点
    tcpinfo *DummyNode = Create_tcpinfo(-1, 0); // -1表示这不是一个有效的TCP连接，timeout为0则是最小值，才能作为哨兵挡住其他结点
    if(!DummyNode) {
        perror("Failed to allocate memory for DummyNode");
        close(epoll_fd);
        free(pPOOL);
        free(temp_array);
        free(hashtable);
        return NULL;
    }
    DummyNode->index = 0;                   // 哨兵结点专属位置

    // 初始化内存块
    memset(pPOOL, 0, sizeof(tcppool));
    memset(temp_array, 0, sizeof(tcpinfo*) * 4096); // 指针数组置空
    memset(hashtable, -1, sizeof(int) * 8192); // 哈希表置-1（文件号从0开始，所以用-1比较好判断）

    // 设置TCP连接池的各项初始参数
    pPOOL->count = 0;
    pPOOL->size = 4096;
    pPOOL->head = DummyNode;                // TCP连接池的链表头结点为哨兵结点
    pPOOL->array_tcpinfo = temp_array;      // 指针数组
    pPOOL->array_tcpinfo[0] = DummyNode;    // 哨兵节点放到哨兵的位置
    pPOOL->hash_socket_tcpinfo = hashtable; // 数组直接用作哈希表
    pPOOL->epoll_fd = epoll_fd;             // 连接池拥有的epoll实例
    pthread_mutex_init(&pPOOL->mutex, NULL);    // 连接池的线程互斥锁

    // 返回设置好的连接池指针
    return pPOOL;
}

void DestroyTCPPool(tcppool* pPOOL) {
    if(!pPOOL) {
        return;
    }
    close(pPOOL->epoll_fd);
    free(pPOOL->array_tcpinfo);
    free(pPOOL->hash_socket_tcpinfo);
    // 涉及增删链表结点的操作，必须使用 list_for_each_safe
    struct list_node *pos = NULL;
    struct list_node *n = NULL;
    list_for_each_safe(pos, n, &pPOOL->head->node) {
        list_del(pos);
        Destroy_tcpinfo(list_entry(pos, tcpinfo, node));
    }
    Destroy_tcpinfo(pPOOL->head);
    memset(pPOOL, 0, sizeof(tcppool));
    free(pPOOL);
}

// 连接池扩容方法，暂时只考虑扩容，不考虑缩小容量
void TCPPool_Expand(tcppool *pPOOL) {
    if(!pPOOL || !pPOOL->array_tcpinfo || !pPOOL->hash_socket_tcpinfo) {
        return;
    }
    // 扩容策略就是最简单的两倍扩容
    int newsize = pPOOL->size * 2;
    tcpinfo** newPtrArray = (tcpinfo**)malloc(sizeof(tcpinfo*) * newsize);
    if (!newPtrArray) {
        // 扩容失败，没办法
        return;
    }
    // 哈希表大小一直都是tcpinfo指针数组的两倍
    int* new_hashtable = (int*)malloc(sizeof(int) * newsize * 2);
    if (!new_hashtable) {
        // 扩容失败，没办法
        free(newPtrArray);
        return;
    }
    // 初始化
    memset(newPtrArray, 0, sizeof(tcpinfo*) * newsize);
    memset(new_hashtable, -1, sizeof(int) * newsize * 2);

    //拷贝信息
    for(int i = 0; i < pPOOL->size; i++) {
        newPtrArray[i] = pPOOL->array_tcpinfo[i];
    }
    for(int i = 0; i < pPOOL->size * 2; i++) {
        new_hashtable[i] = pPOOL->hash_socket_tcpinfo[i];
    }
    //信息转移成功，销毁旧数组，设置新数组
    free(pPOOL->array_tcpinfo);
    free(pPOOL->hash_socket_tcpinfo);

    pPOOL->array_tcpinfo = newPtrArray;
    pPOOL->hash_socket_tcpinfo = new_hashtable;

    // 重设容量
    pPOOL->size = newsize;
}

/**
 * 将一个TCP连接信息元素加入TCPPool中
 * @param pPOOL TCPPool指针
 * @param pInfo TCP连接信息指针
 * @return 失败返回0，成功返回1
 */
int Add_tcpinfo_from_to_TCPPool(tcppool *pPOOL, tcpinfo* pInfo) {
    if (pPOOL == NULL || pInfo == NULL) {
        // 连接池不存在添加失败，pInfo为空添加失败
        return 0;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pPOOL->mutex)) {
        // 加锁失败，连接池出问题了
        return 0;
    }

    // 先检查是否已满，如果满了要扩容
    if (pPOOL->count == pPOOL->size) {
        TCPPool_Expand(pPOOL);
    }
    // 存到链表上
    list_add_tail(&pInfo->node, &pPOOL->head->node);

    pPOOL->count++; //扩大堆规模

    pInfo->timeout = 60;     // 超时时间目前统一为60，后续可以配置化

    // 插入堆底位置
    pInfo->index = pPOOL->count;
    pPOOL->array_tcpinfo[pInfo->index] = pInfo;
    swim(pPOOL, pInfo->index);   // 上浮调整堆结构

    // 调整完毕之后，在哈希表中记录socket文件号和在指针数组中的下标的关系
    pPOOL->hash_socket_tcpinfo[pInfo->client_socket] = pInfo->index;

    // 约定，所有accept队列中的socket都没有加入监听
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;              // 监听读取事件，触发模式为边缘触发
    ev.data.fd = pInfo->client_socket;           // 监听的文件描述符

    // 尝试加入epoll监听，如果监听不成功说明有问题
    if (epoll_ctl(pPOOL->epoll_fd, EPOLL_CTL_ADD, pInfo->client_socket, &ev) == -1) {
        // 无法监听，添加失败
        perror("Failed to add socket to epoll");
    }

    pthread_mutex_unlock(&pPOOL->mutex);            // 解锁
    return 1;
}

/**
 * 将一个tcpinfo从TCPPool中移除
 * @param pPOOL TCPPool指针
 * @param socket_fd socket的文件描述符
 * @return 移除的tcpinfo指针
 */
tcpinfo *Remove_tcpinfo_from_TCPPool(tcppool *pPOOL, int socket_fd) {
    if (pPOOL == NULL || socket_fd < 0) {
        // 连接池不存在操作失败，socket_fd小于0则失败
        return NULL;
    }

    // trylock 返回0说明之前锁没锁定，尝试加锁成功了，否则就返回非0
    if (pthread_mutex_trylock(&pPOOL->mutex)) {
        // 加锁失败，连接池出问题了
        return NULL;
    }

    // 找到这个fd在指针数组中的位置
    int tempIndex = pPOOL->hash_socket_tcpinfo[socket_fd];
    if (tempIndex < 0) {
        // 小于0说明不对，哈希表中没有，无法操作
        return NULL;
    }
    tcpinfo *res = pPOOL->array_tcpinfo[tempIndex];

    list_del(&(res->node));                     // 从存储链表中删除这个TCP连接
    pPOOL->hash_socket_tcpinfo[res->client_socket] = -1;    // 删除这个socket文件号与指针数组中的下标的关系

    exchange(pPOOL, tempIndex, pPOOL->count);   // 把指针交换到堆尾元素
    pPOOL->count--;                             // 缩减堆的规模
    sink(pPOOL, tempIndex);                     // 然后调整堆结构（因为是从堆底交换过来的，所以要下沉），完成堆的性质维护

    // 而后从epoll队列中移除
    if (epoll_ctl(pPOOL->epoll_fd, EPOLL_CTL_DEL, res->client_socket, NULL) == -1) {
        // 无法从epoll队列移除，打error
        perror("Failed to remove socket from epoll");
    }

    pthread_mutex_unlock(&pPOOL->mutex);   // 解锁
    return res;
}

// 程序是否继续运行标志
int keep_running;

tcpinfo **all_tcpinfo_hashtable = NULL;     // all_tcpinfo_hashtable[client_socket] = 对应的tcpinfo指针
int all_tcpinfo_hashtable_size = 4096;

typedef struct {
    int port;                       // 监听端口
    int max_backlog;                // 允许系统为该监听套接字挂起的未完成连接的最大数量，即listen函数的第二个参数
    // 后续可以继续添加其他配置项，如日志级别、超时时间等
} ServerConfig;

// 设置文件为非阻塞模式
int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1; // 处理错误
    }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1) {
        return -1; // 处理错误
    }
    return 0;
}

// 监听端口线程
void *ListenPortThread(void *arg) {
    if(!arg) {
        pthread_exit(NULL);
    }

    ServerConfig *config = (ServerConfig *)arg;

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
    server_addr.sin_port = htons(config->port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Socket bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    //将server_socket设为非阻塞模式，用select，要不然一直阻塞在那里线程退不出来
    set_non_blocking(server_socket);

    if (listen(server_socket, config->max_backlog) < 0) {
        perror("Socket listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    fd_set readfds;

    struct timeval tv = {0, 200 * 1000};  // 200ms timeout

    while (keep_running) {
        // select会改变fd_set的具体内容，所以每次select之前都要重新初始化fd_set
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;  // 每次都要重设 timeval 结构

        int ret = select(server_socket + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select failed on server_socket");
            continue;
        } else if (ret == 0) {
            continue;  // timeout
        }

        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Client accept failed");
            continue;
        }

        set_non_blocking(client_socket);

        if (client_socket >= all_tcpinfo_hashtable_size) {
            tcpinfo ** new_all_tcpinfo_hashtable = (tcpinfo**)malloc(sizeof(tcpinfo*) * all_tcpinfo_hashtable_size * 2);
            if(!new_all_tcpinfo_hashtable) {
                perror("cannot expand all_tcpinfo_hashtable");
                exit(1);
            }
            memset(new_all_tcpinfo_hashtable, 0, sizeof(tcpinfo*) * all_tcpinfo_hashtable_size * 2);
            for(int i = 0; i < all_tcpinfo_hashtable_size; i++) {
                new_all_tcpinfo_hashtable[i] = all_tcpinfo_hashtable[i];
            }
            all_tcpinfo_hashtable = new_all_tcpinfo_hashtable;
            all_tcpinfo_hashtable_size *= 2;
        }

        all_tcpinfo_hashtable[client_socket] = Create_tcpinfo(client_socket, 0);

        Add_tcpinfo_to_queue(accept_queue, all_tcpinfo_hashtable[client_socket]);
    }

    // 关闭所有连接
    for(int i = 0; i < all_tcpinfo_hashtable_size; i++) {
        if (all_tcpinfo_hashtable[i]) {
            Destroy_tcpinfo(all_tcpinfo_hashtable[i]);
        }
    }
    memset(all_tcpinfo_hashtable, 0, sizeof(tcpinfo*) * all_tcpinfo_hashtable_size);


    shutdown(server_socket, SHUT_RDWR);  // 关闭读写
    close(server_socket);  // 关闭套接字

    free(config);       // 释放配置文件内存

    pthread_exit(NULL);
}

// 秒标志，每隔一秒变动一次
volatile int second_flag;

// 连接池管理线程函数
void* tcppool_manage_thread(void* arg) {
    tcppool * pPOOL = (tcppool *)arg;

    // 事件数组大小设置为2048，写死了，一次性最多管理这么多
    int events_size = 2048;
    struct epoll_event *events = (struct epoll_event *)malloc(events_size * sizeof(struct epoll_event));

    volatile int local_second_flag = second_flag;

    while (keep_running) {

        if (local_second_flag != second_flag) {
            // 此时已经过了一秒钟了，给所有的待管理元素的超时阈值减去1

            // 注意，阈值最小为0，如果已经是0，必须跳过

            struct list_node *pos = NULL;
            list_for_each(pos, &pPOOL->head->node) {
                if (list_entry(pos, tcpinfo, node)->timeout > 0) {
                    list_entry(pos, tcpinfo, node)->timeout--;
                }
            }
            local_second_flag = second_flag;
        }

        // 检查有没有剩余超时时间已经为0的（因为是最小堆，如果堆顶都没超时，其他的更不用说），有的话就全部销毁
        while(pPOOL->count > 0 && pPOOL->array_tcpinfo[1]->timeout == 0) {
            // 从连接池中删除并销毁其连接
            tcpinfo *top_tcpinfo = pPOOL->array_tcpinfo[1];
            // 超时控制中关闭，销毁，要删除其在哈希表中的映射
            all_tcpinfo_hashtable[top_tcpinfo->client_socket] = NULL;
            Destroy_tcpinfo(Remove_tcpinfo_from_TCPPool(pPOOL, top_tcpinfo->client_socket));
        }

        // 如果accept队列里面有数据，就开始判断
        while(!IsEmpty_tcpinfo_queue(accept_queue)) {
            tcpinfo *temp = Remove_tcpinfo_from_queue(accept_queue);
            if(temp) {
                // 检查是否有数据需要处理
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(temp->client_socket, &readfds);

                // 设置超时为0，使select立即返回
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;

                int ret = select(temp->client_socket + 1, &readfds, NULL, NULL, &timeout);

                if (ret > 0) {
                    // 有数据，立即送到release_queue，等待发送
                    Add_tcpinfo_to_queue(release_queue, temp);
                } else if (ret == 0) {
                    // 没有数据，入池管理，默认超时时间60秒
                    Add_tcpinfo_from_to_TCPPool(pPOOL, temp);
                } else {
                    // 这个出错了，只能销毁
                    Destroy_tcpinfo(temp);
                }
            }
        }

        // 使用 epoll_wait 函数等待事件发生
        // epoll_fd：epoll 实例的文件描述符
        // events：用于存储发生的事件的数组
        // events_size：events 数组的大小
        // timeout：指定超时时间为-1，除非有事件发生，否则一直阻塞，如果为0则是立即返回的非阻塞状态，如果是正数则是阻塞时长
        // 返回结果 nfds 表示的发生事件的数量
        int nfds = epoll_wait(pPOOL->epoll_fd, events, events_size, 0);

        if (nfds == -1) {
            // 检查 epoll_wait 是否失败
            perror("epoll_wait failed");
            continue;  // 如果失败，继续循环（在实际代码中可以考虑不同的处理方式）
        }

        for (int n = 0; n < nfds; ++n) {
            // 判断当前事件是否是可读事件（EPOLLIN 表示数据可读）
            if (events[n].events & EPOLLIN) {
                // 获取发生事件的文件描述符（即有数据可读的 socket）
                int fd = events[n].data.fd;
                // 找到对应的指针
                tcpinfo *pInfo = Remove_tcpinfo_from_TCPPool(pPOOL, fd);
                if (pInfo != NULL) {
                    Add_tcpinfo_to_queue(release_queue, pInfo);
                }
            }
        }
    }
    //释放整个TCP管理体系
    DestroyTCPPool(pPOOL);

    pthread_exit(NULL);
}

// 与管理进程交互的Socket
int send_to_Manager_Process_Socket = -1;

// 从工作进程接收处理完毕的Socket
int recv_from_Worker_Process_Socket_keep_alive = -1;
int recv_from_Worker_Process_Socket_close = -1;

// 将release队列中的tcp连接信息发到管理进程
void *SendToManagerThread(void *arg) {
    while (keep_running) {
        tcpinfo *pInfo = Remove_tcpinfo_from_queue(release_queue);
        if (pInfo) {
            if (fd_send(send_to_Manager_Process_Socket, pInfo->client_socket) < 0) {
                perror("[ERROR] Failed to send fd to Manager");
            }
            // 不能销毁，先留在监听进程中打开
        } else {
            sched_yield();  // 没数据就让出CPU
        }
    }

    pthread_exit(NULL);
}


#include <dirent.h>

#define MAX_PATH_LENGTH 1024

// Function to check if the socket is being used by any process
int check_socket_usage(int target_fd) {
    char proc_fd_path[MAX_PATH_LENGTH];
    char link_target[MAX_PATH_LENGTH];
    struct dirent *entry;
    DIR *dir;
    int count = 0;

    // Traverse the /proc directory to check file descriptors of all processes
    dir = opendir("/proc");
    if (!dir) {
        perror("opendir");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        // Check if the entry is a valid PID directory
        if (entry->d_type == DT_DIR && atoi(entry->d_name) > 0) {
            // Construct the path to the /proc/[pid]/fd directory
            snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/%s/fd", entry->d_name);

            // Open the fd directory for the current process
            DIR *fd_dir = opendir(proc_fd_path);
            if (fd_dir) {
                struct dirent *fd_entry;
                while ((fd_entry = readdir(fd_dir)) != NULL) {
                    if (fd_entry->d_type == DT_LNK) {
                        // Construct the full path for the symbolic link
                        char fd_link[MAX_PATH_LENGTH];
                        snprintf(fd_link, sizeof(fd_link), "%s/%s", proc_fd_path, fd_entry->d_name);

                        // Get the target of the symbolic link
                        ssize_t len = readlink(fd_link, link_target, sizeof(link_target) - 1);
                        if (len != -1) {
                            link_target[len] = '\0';

                            // Check if the link target corresponds to a socket
                            if (strstr(link_target, "socket:[") != NULL) {
                                int fd;
                                sscanf(link_target, "socket:[%d]", &fd);
                                if (fd == target_fd) {
                                    printf("Found socket %d in process %s with fd %s\n", target_fd, entry->d_name, fd_entry->d_name);
                                    count++;
                                    break;  // Found the socket in this process, no need to continue checking its other fds
                                }
                            }
                        }
                    }
                }
                closedir(fd_dir);
            }
        }
    }

    closedir(dir);
    return count;
}

// 从工作进程接收socket，放入accept队列中，等待进入连接池
void *RecvFromWorkerThread(void *arg) {
    struct timeval tv;
    fd_set read_fds;

    while (keep_running) {
        // 检查keep-alive
        FD_ZERO(&read_fds);
        FD_SET(recv_from_Worker_Process_Socket_keep_alive, &read_fds);

        tv.tv_sec = 0;
        tv.tv_usec = 0; // 非阻塞检查

        int ret = select(recv_from_Worker_Process_Socket_keep_alive + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(recv_from_Worker_Process_Socket_keep_alive, &read_fds)) {
            int client_fd = fd_recv(recv_from_Worker_Process_Socket_keep_alive);
            if (client_fd >= 0) {
                tcpinfo *pInfo = all_tcpinfo_hashtable[client_fd];
                if (pInfo) {
                    Add_tcpinfo_to_queue(accept_queue, pInfo);
                } else {
                    close(client_fd);   // 这个连接来源有问题
                }
            }
        }

        // 检查close
        FD_ZERO(&read_fds);
        FD_SET(recv_from_Worker_Process_Socket_close, &read_fds);

        tv.tv_sec = 0;
        tv.tv_usec = 0; // 非阻塞检查

        ret = select(recv_from_Worker_Process_Socket_close + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(recv_from_Worker_Process_Socket_close, &read_fds)) {
            int client_fd = fd_recv(recv_from_Worker_Process_Socket_close);
            if (client_fd >= 0) {
                tcpinfo *pInfo = all_tcpinfo_hashtable[client_fd];
                if (pInfo) {
                    // 需要关闭，删除其在哈希表中的映射
                    all_tcpinfo_hashtable[client_fd] = NULL;
                    Destroy_tcpinfo(pInfo);
                } else {
                    close(client_fd);   // 这个连接来源有问题
                }
            }
        }

        usleep(100 * 1000); // 100ms轮询一次
    }

    pthread_exit(NULL);
}

// 这是与主进程（守护进程）交互的Socket（用于接收与管理进程交互的两个socket，以及上报心跳包）
int Daemon_Main_Socket = -1;

int main(int argc, char *argv[]) {
    // 参数校验
    if (argc < 2) {
        fprintf(stderr, "[ERROR] No file descriptor passed as argument\n");
        exit(1);
    }

    Daemon_Main_Socket = atoi(argv[1]);  // 将传递的文件描述符转换为整数

    // 使用传递过来的文件描述符，进行后续的监听或通信
    printf("[INFO] Listener Received Daemon_Main_Socket: %d\n", Daemon_Main_Socket);

    // 如果能顺利连接，就能接收到守护进程发来的，给监听进程与管理进程的交互的Socket
    send_to_Manager_Process_Socket = fd_recv(Daemon_Main_Socket);

    // 两个缺一不可
    if (send_to_Manager_Process_Socket < 0) {
        fprintf(stderr, "Failed to connect to Manager_Process.\n");
        return -1;
    }
    recv_from_Worker_Process_Socket_keep_alive = fd_recv(Daemon_Main_Socket);
    recv_from_Worker_Process_Socket_close = fd_recv(Daemon_Main_Socket);
    if ((recv_from_Worker_Process_Socket_keep_alive < 0) || (recv_from_Worker_Process_Socket_close < 0)) {
        fprintf(stderr, "Failed to connect to Worker_Process.\n");
        return -1;
    }

    keep_running = 1;
    second_flag = 0;

    all_tcpinfo_hashtable = (tcpinfo**)malloc(sizeof(tcpinfo*) * all_tcpinfo_hashtable_size);
    if(!all_tcpinfo_hashtable) {
        fprintf(stderr, "Failed to create all_tcpinfo_hashtable.\n");
        return -1;
    }
    memset(all_tcpinfo_hashtable, 0, sizeof(tcpinfo*) * all_tcpinfo_hashtable_size);

    // 创建工作队列
    accept_queue = Create_tcpinfo_queue();
    release_queue = Create_tcpinfo_queue();

    if (!accept_queue || !release_queue) {
        fprintf(stderr, "Failed to create queues.\n");
        return -1;
    }

    // 创建TCP连接池
    tcppool *pPOOL = CreateTCPPool();
    if (!pPOOL) {
        fprintf(stderr, "Failed to create TCP pool.\n");
        return -1;
    }

    // 初始化监听配置
    ServerConfig *config = (ServerConfig *)malloc(sizeof(ServerConfig));
    config->port = 8080;
    config->max_backlog = 1024;

    // 既然主线程没事干，就承担计时和上报的功能

    // 启动所有线程
    pthread_t listen_port_thread, manage_thread;
    pthread_create(&listen_port_thread, NULL, ListenPortThread, config);
    pthread_create(&manage_thread, NULL, tcppool_manage_thread, pPOOL);

    pthread_t recv_thread, send_thread;
    pthread_create(&recv_thread, NULL, RecvFromWorkerThread, NULL);
    pthread_create(&send_thread, NULL, SendToManagerThread, NULL);

    struct timespec ts_target, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_target);

    // 监听进程要上报给守护进程的信息
    StatusMessage status;
    // 约定使用参数顺序如下
    // StatusMessage.status_args[] 中顺序如下：
    // [0] = 模块号（固定：1 = Listener）
    // [1] = PID
    // [2] = 连接池容量
    // [3] = 当前连接池中的连接数
    // [4] = 堆顶元素超时时间
    // [5] = accept_queue 中等待入池的连接数
    // [6] = release_queue 中等待发出的连接数

    // 守护进程发来的控制信息
    StatusMessage control_msg;
    memset(&control_msg, 0, sizeof(StatusMessage));

    printf("[INFO] WebServer_Listener started.\n");

    while (keep_running) {
        // 目标时间：每次加1秒
        ts_target.tv_sec += 1;

        // 等待直到目标时间
        while (keep_running) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);

            // 如果已达到或超过目标时间，跳出
            if ((ts_now.tv_sec > ts_target.tv_sec) ||
                (ts_now.tv_sec == ts_target.tv_sec && ts_now.tv_nsec >= ts_target.tv_nsec)) {
                break;
            }

            // 否则主动让出CPU
            sched_yield();
        }

        // 更新计时标志
        second_flag ^= 1;  // 翻转标志（0和1异或得1，1和1异或得0）

        // === 使用 select 等待可读 ===
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(Daemon_Main_Socket, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;  // 非阻塞 select，仅检查状态

        int sel_ret = select(Daemon_Main_Socket + 1, &readfds, NULL, NULL, &timeout);
        if (sel_ret < 0) {
            perror("[ERROR] select failed");
            break;
        } else if (sel_ret == 0) {
            // 没有数据，跳到发送心跳包
            goto heartbeat_report;
        }

        int ret = recv_status_message(Daemon_Main_Socket, &control_msg);

        /**
         * shutdown_msg的魔数为：
         * args[0] = -25;
         * args[3] = -41;
         * args[10] = -99;
         */

        if (ret == 0 && control_msg.args[0] == -25 && control_msg.args[3] == -41 && control_msg.args[10] == -99) {
            printf("[INFO] Listener received shutdown signal.\n");
            keep_running = 0;
            break;
        }

heartbeat_report:
        // 发送信息（也起到心跳包的作用）
        memset(&status, 0, sizeof(status));
        status.module = 1; // 监听模块编号
        status.pid = getpid();
        status.args[0] = pPOOL->size;
        status.args[1] = pPOOL->count;
        if (pPOOL->count > 0) {
            status.args[2] = (int)(pPOOL->array_tcpinfo[1]->timeout);
        } else {
            status.args[2] = 0;
        }
        status.args[3] = accept_queue->size;
        status.args[4] = release_queue->size;

        if (send_status_message(Daemon_Main_Socket, &status) < 0) {
            perror("Failed to send status to Daemon");
        }
    }

    // 等待线程全部退出
    pthread_join(listen_port_thread, NULL);
    pthread_join(manage_thread, NULL);
    pthread_join(recv_thread, NULL);
    pthread_join(send_thread, NULL);


    // 销毁资源
    Destroy_tcpinfo_queue(accept_queue);
    Destroy_tcpinfo_queue(release_queue);

    // 清理自己持有的所有socket
    close(send_to_Manager_Process_Socket);
    close(recv_from_Worker_Process_Socket_keep_alive);
    close(recv_from_Worker_Process_Socket_close);
    close(Daemon_Main_Socket);

    printf("[INFO] WebServer_Listener exit.\n");

    return 0;
}
