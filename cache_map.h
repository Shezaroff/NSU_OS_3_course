#ifndef __CACHE_MAP_H__
#define __CACHE_MAP_H__

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>  

#include "http_request.h"
#include "dynamic_buffer.h"

#define MAX_SIZE_CACHE_NODE (1ULL * 1024 * 1024 * 1024)
#define MAX_SIZE_CACHE_MAP (2ULL * 1024 * 1024 * 1024)

typedef enum {
    IN_PROGRESS,
    DONE,
    PASS
} cache_node_state;

typedef struct Cache_Reader {
    int socket;
    size_t offset;    
    int dead;

    struct Cache_Reader* next;
} Cache_Reader;

typedef struct Cache_Node {
    char* key;
    dynbuf response;
    size_t recv_cnt;
    cache_node_state state;

    int eof;
    int error;

    ssize_t content_length;
    size_t base_offset;

    Cache_Reader* readers;
    uint32_t readers_num;

    // _Atomic uint32_t hits;

    pthread_mutex_t mutex;
    pthread_cond_t cond_var;

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

int alloc_cache_node(Cache_Node** node, const char* key);

void destroy_cache_node(Cache_Node** node);

int get_set_cache_map(Cache_Map* map, const char* key, 
                      Cache_Node** out_node, int* created);

int add_reader_cache_node(Cache_Node* node, Cache_Reader** reader/*,int socket*/);

void remove_reader_cache_node(Cache_Node* node, Cache_Reader** reader);

void trim_cache_node(Cache_Node* node);

int stream_from_cache_node(Cache_Node* node, Cache_Reader *reader);

int build_cache_key(char* dst, size_t cap,
                    const char* host, const char* port,
                    const http_request* req);

#endif