#ifndef __CACHE_MAP_H__
#define __CACHE_MAP_H__

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#include "http_request.h"

#define MAX_SIZE_CACHE_NODE (1ULL * 1024 * 1024 * 1024)
#define MAX_SIZE_CACHE_MAP (2ULL * 1024 * 1024 * 1024)

typedef struct Cache_Node {
    char* key;
    char* response;
    size_t size;

    _Atomic uint32_t hits;

    struct Cache_Node* next;
} Cache_Node;

typedef struct Cache_Map {
    Cache_Node* first;
    size_t total_size;
    _Atomic uint32_t num_requests;
    pthread_rwlock_t lock;
} Cache_Map;

void init_cache_map(Cache_Map* map);

void destroy_cache_map(Cache_Map* map);

int get_cache_map(Cache_Map* map, const char* key, char** out, size_t* out_size);

int alloc_cache_node(Cache_Node** node);

void destroy_cache_node(Cache_Node** node);

int add_cache_map(Cache_Map* map, const char* key, const char* response, size_t size);

int build_cache_key(char* dst, size_t cap,
                    const char* host, const char* port,
                    const http_request* req);

#endif