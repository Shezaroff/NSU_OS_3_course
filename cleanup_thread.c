#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#include "cleanup_thread.h"

/**
 * Функция для запуска в потоке.
 * Раз в cache_cleaner_args.interval_sec секунд вызывает функцию отчистки кэша.
 */
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

/**
 * Функция для сравнения записей кэшей по количеству обращений.
 */
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

/**
 * При достижении percent_for_del от максимального объема кэша производит удаление записей.
 * Пытается отчистить как минимум треть записей с наименьшим количеством обращений с прошлой отчистки.
 * Отчищает только завершенные записи с истекшим ttl, те у которых нет читателей и нет активных ссылок.
 */
int delete_cache(Cache_Map *map, size_t max_size_bytes, size_t percent_for_del) {
    if (map == NULL || percent_for_del > 100) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&map->lock);

    size_t total_size = 0, n = 0;
    Cache_Node* current = map->first;
    while (current != NULL) {
        current->ttl--;
        n++;
        pthread_mutex_lock(&current->mutex);
        total_size += current->response.len;
        pthread_mutex_unlock(&current->mutex);
        current = current->next;
    }
    
    if (n == 0) {
        pthread_rwlock_unlock(&map->lock);
        return 0;
    }

    if (total_size < (max_size_bytes * percent_for_del) / 100) {
        pthread_rwlock_unlock(&map->lock);
        return 0; 
    }
    
    uint32_t num_reqs = map->num_requests;
    if (num_reqs == 0) {
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
    if (k == 0 && n > 0) {
        k = 1;
    }

    uint32_t cutoff = atomic_load(&arr[k - 1]->hits);

    Cache_Node **prev_ptr = &map->first;
    while (*prev_ptr) {
        Cache_Node *cur = *prev_ptr;
        uint32_t h = atomic_load(&cur->hits);

        pthread_mutex_lock(&cur->mutex);

        if (h <= cutoff && cur->ttl <= 0 && cur->state != IN_PROGRESS && cur->readers_num == 0 && atomic_load(&cur->ref_cnt) == 0) {
            *prev_ptr = cur->next;
            pthread_mutex_unlock(&cur->mutex);
            destroy_cache_node(&cur);
            continue;
        }

        atomic_store(&cur->hits, 0);
        prev_ptr = &cur->next;
    }

    map->num_requests = 0;
    free(arr);
    pthread_rwlock_unlock(&map->lock);
    return 0;
}
