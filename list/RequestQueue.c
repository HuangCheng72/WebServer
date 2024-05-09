//
// Created by huangcheng on 2024/5/8.
//

#include "RequestQueue.h"

// 创建一个完整的请求
RequestData *CreateRequestData() {
    RequestData *pRequestData = (RequestData *)malloc(sizeof(RequestData));
    init_list_node(&(pRequestData->list));
    pRequestData->data = NULL;
    pRequestData->length = 0;
    return pRequestData;
}

// 完全回收请求数据
void FreeRequestData(RequestData *pData) {
    if (pData) {
        free(pData->data);
        pData->data = NULL;
        free(pData);
    }
}

// 创建一个完整的请求队列
RequestQueue *CreateRequestQueue() {
    RequestQueue *pRequestQueue = (RequestQueue *)malloc(sizeof(RequestQueue));
    init_list_node(&pRequestQueue->head);
    return pRequestQueue;
}

// 入队操作，将请求数据添加到队列中
void AddToRequestQueue(RequestQueue *pRequestQueue, char *data, int length) {
    RequestData *req_data = CreateRequestData();
    req_data->data = (char *)malloc(length);
    memcpy(req_data->data, data, length);
    req_data->length = length;
    list_add_tail(&req_data->list, &pRequestQueue->head);
}

// 出队操作，从队列中获取一个请求数据
RequestData *GetFromRequestQueue(RequestQueue *pRequestQueue) {
    if (list_empty(&pRequestQueue->head)) {
        return NULL;
    }
    LIST_NODE *node = pRequestQueue->head.next;
    list_del(node);
    return list_entry(node, RequestData, list);
}

// 检查队列是否为空
int IsRequestQueueEmpty(RequestQueue *pRequestQueue) {
    return list_empty(&pRequestQueue->head);
}
