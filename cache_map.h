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
#define DEFAULT_TTL 16

/**
 * Описывает состояние записи о кэше.
 */
typedef enum {
    IN_PROGRESS,
    DONE,
    PASS
} cache_node_state;

/**
 * Описывает читателя конкретной записи в кэше.
 */
typedef struct Cache_Reader {
    int socket;
    size_t offset;    
    int dead;

    struct Cache_Reader* next;
} Cache_Reader;

/**
 * Описывает кокнретную записб в кэше.
 */
typedef struct Cache_Node {
    char* key;
    dynbuf response;
    size_t recv_cnt; // Сколько всего байт получено от хоста за все врем
    cache_node_state state;

    _Atomic uint32_t hits; // Счетчик обращений к ноде
    uint32_t ttl; // Количество итераций, которые поток отчистки не будет удалять эту запись
    _Atomic uint32_t ref_cnt; // Количество тредов, хранящих ссылку на ноду в данный момент

    // Чтение завершено по этой причине
    int eof;
    int error;

    // ответ не нужно кэшировать, при этом он раздавался уже подключившимся клиентам,
    // но все соединения были закрыты и пора освобождать память 
    int abort_pass;
    // тело кэша (ответ) освобожден
    int response_freed;

    ssize_t content_length;
    size_t base_offset; // Текущий сдвиг относительно начала данных

    Cache_Reader* readers;
    uint32_t readers_num;

    pthread_mutex_t mutex;
    pthread_cond_t cond_var;

    struct Cache_Node* next;
} Cache_Node;

/**
 * Описывает хранилище кэша.
 */
typedef struct Cache_Map {
    Cache_Node* first;
    // size_t total_size;
    uint32_t num_requests;
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