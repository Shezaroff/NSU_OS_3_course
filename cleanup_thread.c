#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#include "cleanup_thread.h"


void* cache_cleaner_thread(void *arg) {
    cache_cleaner_args *a = (cache_cleaner_args*)arg;
    if (a == NULL || a->map == NULL) {
        return NULL;
    }

    while (1) {
        sleep(a->interval_sec);

        delete_cache(a->map, a->max_size_bytes, a->percent_for_del);
    }
    return NULL;
}

static int cmp_hits_asc(const void *a, const void *b) {
    const Cache_Node *na = *(const Cache_Node * const *)a;
    const Cache_Node *nb = *(const Cache_Node * const *)b;

    uint32_t ha = atomic_load_explicit(&na->hits, memory_order_relaxed);
    uint32_t hb = atomic_load_explicit(&nb->hits, memory_order_relaxed);

    if (ha < hb) {
        return -1;
    }
    if (ha > hb) {
        return 1;
    }
    return 0;
}

int delete_cache(Cache_Map *map, size_t max_size_bytes, size_t percent_for_del) {
    if (map == NULL || percent_for_del > 100) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&map->lock);

    if (map->total_size < (max_size_bytes * percent_for_del) / 100) {
        pthread_rwlock_unlock(&map->lock);
        return 0; 
    }
    
    uint32_t num_reqs = atomic_load_explicit(&map->num_requests, memory_order_relaxed);
    if (num_reqs == 0) {
        pthread_rwlock_unlock(&map->lock);
        return 0;
    }

    size_t n = 0;
    Cache_Node* current = map->first;
    while(current != NULL) {
        n++;
        current = current->next;
    }

    if (n == 0) {
        pthread_rwlock_unlock(&map->lock);
        return 0;
    }

    Cache_Node **arr = malloc(n * sizeof(*arr));
    if (!arr) {
        pthread_rwlock_unlock(&map->lock);
        return -1;
    }

    size_t i = 0;
    current = map->first;
    while(current != NULL) {
        arr[i] = current;
        i++;
        current = current->next;
    }

    qsort(arr, n, sizeof(*arr), cmp_hits_asc);

    size_t k = n / 3;
    if (k == 0 && n > 0) k = 1;

    uint32_t cutoff = atomic_load_explicit(&arr[k - 1]->hits, memory_order_relaxed);

    Cache_Node **pp = &map->first;
    while (*pp) {
        Cache_Node *cur = *pp;
        uint32_t h = atomic_load_explicit(&cur->hits, memory_order_relaxed);

        if (h <= cutoff) {
            *pp = cur->next;
            map->total_size -= cur->size;
            destroy_cache_node(&cur);
            continue;
        }

        atomic_store_explicit(&cur->hits, 0, memory_order_relaxed);
        pp = &cur->next;
    }

    atomic_store_explicit(&map->num_requests, 0, memory_order_relaxed);
    free(arr);
    pthread_rwlock_unlock(&map->lock);
    return 0;
}
