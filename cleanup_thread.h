#ifndef __CLEANUP_THREAD_H__
#define __CLEANUP_THREAD_H__

#include <stdlib.h>
#include "cache_map.h"

typedef struct {
    Cache_Map *map;
    size_t max_size_bytes;        
    unsigned long interval_sec;        
    size_t percent_for_del;         
} cache_cleaner_args;

void* cache_cleaner_thread(void *arg);

int delete_cache(Cache_Map *map, size_t max_size_bytes, size_t percent_for_del);

#endif