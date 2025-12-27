#ifndef __HOST_RECEIVER_THREAD_H__
#define __HOST_RECEIVER_THREAD_H__

#include <stdlib.h>
#include "cache_map.h"

typedef struct {
    Cache_Node* cache_node;
    char* host;
    char* port;
    char* req_data;
    size_t req_len;
} receiver_args;

void add_cache_node(Cache_Node* node, const void* data, size_t n);

void* reciever_thread(void* arg);

#endif