#include <string.h>
#include <stdio.h>
#include "cache_map.h"
#include "http_request.h"
#include "http_utils.h"

void init_cache_map(Cache_Map* map) {
    if (map == NULL) {
        return;
    }

    map->first = NULL;
    map->total_size = 0;
    map->num_requests = 0;
    
    pthread_rwlock_init(&map->lock, NULL);
}

void destroy_cache_map(Cache_Map* map) {
    if (map == NULL) {
        return;
    } 
    Cache_Node* current = map->first, *tmp;
    while (current != NULL) {
        tmp = current->next;
        destroy_cache_node(&current);
        current = tmp;
    }
    map->first = NULL;
    map->total_size = 0;
    pthread_rwlock_destroy(&map->lock);
}

int get_cache_map(Cache_Map* map, const char* key, char** out, size_t* out_size) {
    if (map == NULL || key == NULL) {
        return -1;
    }

    pthread_rwlock_rdlock(&map->lock);
    Cache_Node* current = map->first;
    
    atomic_fetch_add_explicit(&map->num_requests, 1, memory_order_relaxed);

    while (current != NULL) {
        if (strcmp(key, current->key) == 0) {
            atomic_fetch_add_explicit(&current->hits, 1, memory_order_relaxed);
            if (out != NULL && out_size != NULL) {
                char* buf = malloc(current->size);
                if (buf == NULL) { 
                    pthread_rwlock_unlock(&map->lock);
                    return -1; 
                }
                memcpy(buf, current->response, current->size);
                *out = buf;
                *out_size = current->size;
            }
            pthread_rwlock_unlock(&map->lock);
            return 0;
        }
        // tmp = current->next;
        current = current->next;
    }

    pthread_rwlock_unlock(&map->lock);

    return 1;
}

int alloc_cache_node(Cache_Node** node) {
    if (node == NULL) {
        return -1;
    }
    *node = malloc(sizeof(Cache_Node));
    if (*node == NULL) {
        return -1;
    }

    (*node)->key = NULL;
    (*node)->response = NULL;
    (*node)->size = 0;
    (*node)->next = NULL;
    (*node)->hits = 0;
    // pthread_rwlock_init(&(*node)->lock, NULL);
    return 0;
} 

void destroy_cache_node(Cache_Node** node) {
    if (node == NULL || *node == NULL) {
        return;
    }

    if ((*node)->key != NULL) {
        free((*node)->key);
    } 
    if ((*node)->response != NULL) {
        free((*node)->response);
    } 

    // pthread_rwlock_destroy(&(*node)->lock);

    free(*node);
    *node = NULL;
}

int add_cache_map(Cache_Map* map, const char* key, const char* response, size_t size) {
    if (map == NULL || key == NULL || response == NULL || size > MAX_SIZE_CACHE_NODE) {
        return -1;
    }
    
    Cache_Node* node;
    if (alloc_cache_node(&node) == -1) {
        return -1;
    }

    node->key = strdup(key);
    if (node->key == NULL) {
        destroy_cache_node(&node);
        return -1;
    }
    node->response = malloc(size);
    if (node->response == NULL) {
        destroy_cache_node(&node);
        return -1;
    }
    memcpy(node->response, response, size);
    node->size = size;

    // if (get_cache_map(map, key, NULL, NULL) == 0) {
    //     return -1;
    // }

    pthread_rwlock_wrlock(&map->lock);

    if (map->total_size + size > MAX_SIZE_CACHE_MAP) {
        pthread_rwlock_unlock(&map->lock);
        destroy_cache_node(&node);
        return -1;
    }
    
    Cache_Node* current = map->first, *tmp;
    while (current != NULL) {
        tmp = current->next;
        if (strcmp(current->key, key) == 0) {
            pthread_rwlock_unlock(&map->lock);
            destroy_cache_node(&node);
            return -1;
        }
        current = tmp;
    }

    node->next = map->first;
    map->first = node;
    map->total_size += node->size;
    pthread_rwlock_unlock(&map->lock);
    return 0;
}

int build_cache_key(char* dst, size_t cap,
                    const char* host, const char* port,
                    const http_request* req) {
    if (dst == NULL || cap == 0 || host == NULL || port == NULL || req == NULL || req->target_path == NULL) {
        return -1;
    }

    // const char *path = req->target_path;
    const char* path = from_absolute_path(req->target_path, NULL, 0);
    int n = snprintf(dst, cap, "GET %s:%s%s", host, port, path);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}
