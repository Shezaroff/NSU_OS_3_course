#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "cache_map.h"
#include "http_request.h"
#include "http_utils.h"

void init_cache_map(Cache_Map* map) {
    if (map == NULL) {
        return;
    }

    map->first = NULL;
    // map->total_size = 0;
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
    // map->total_size = 0;
    pthread_rwlock_destroy(&map->lock);
}

int alloc_cache_node(Cache_Node** node, const char* key) {
    if (node == NULL || key == NULL) {
        return -1;
    }
    *node = malloc(sizeof(Cache_Node));
    if (*node == NULL) {
        return -1;
    }

    (*node)->key = strdup(key);
    if ((*node)->key == NULL) {
        free((*node));
        return -1;
    }
    (*node)->response.data = NULL;
    (*node)->response.len = 0;
    (*node)->response.cap = 0;

    (*node)->recv_cnt = 0;

    atomic_store(&(*node)->ref_cnt, 0);

    (*node)->ttl = DEFAULT_TTL;

    atomic_store_explicit(&(*node)->hits, 0, memory_order_seq_cst);

    (*node)->eof = 0;
    (*node)->error = 0;

    (*node)->abort_pass = 0;
    (*node)->response_freed = 0;

    (*node)->content_length = -1;
    (*node)->base_offset = 0;

    (*node)->readers = NULL;
    (*node)->readers_num = 0;

    (*node)->state = IN_PROGRESS;

    pthread_mutex_init(&(*node)->mutex, NULL);
    pthread_cond_init(&(*node)->cond_var, NULL);

    (*node)->next = NULL;
    return 0;
} 

void destroy_cache_node(Cache_Node** node) {
    if (node == NULL || *node == NULL) {
        return;
    }

    if ((*node)->key != NULL) {
        free((*node)->key);
    } 
    free_dynbuf(&(*node)->response);

    Cache_Reader* current = (*node)->readers, *tmp;
    while (current != NULL) {
        tmp = current->next;
        free(current);
        current = tmp;
    }

    pthread_mutex_destroy(&(*node)->mutex);
    pthread_cond_destroy(&(*node)->cond_var);

    free(*node);
    *node = NULL;
}

Cache_Node* get_node(Cache_Map* map, const char* key) {
    Cache_Node* current = map->first;
    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            atomic_fetch_add(&current->hits, 1);
            // *out_node = current;
            // atomic_fetch_add(&(current)->ref_cnt, 1);
            // *created = 0;
            // pthread_rwlock_unlock(&map->lock);
            break;;
        }
        current = current->next;
    }

    return current;
}

Cache_Node* create_and_add_new_node(Cache_Map* map, const char* key) {
    Cache_Node* new_node;
    if (alloc_cache_node(&new_node, key) == -1) {
        return NULL;
    }

    pthread_mutex_lock(&new_node->mutex);
    new_node->readers_num = 1;
    atomic_fetch_add(&new_node->hits, 1);
    pthread_mutex_unlock(&new_node->mutex);
    new_node->next = map->first;
    map->first = new_node;
    return new_node;
}

/*
 * Ищет запись в кэше по ключу. Возвращает найденную запись через out_node.
 * Если ключ не найден, создает новую запись и помечает created = true.
 * Захватывает write lock на всю кэш мапу и mutex на ноду в случае создания новой записи.
 */
int get_set_cache_map(Cache_Map* map, const char* key, 
                      Cache_Node** out_node, int* created) {
    if (map == NULL || key == NULL || out_node == NULL || created == NULL) {
        return -1;
    }

    pthread_rwlock_wrlock(&map->lock);
    
    map->num_requests++;

    Cache_Node* node_in_map = get_node(map, key);

    Cache_Node* new_node = NULL;
    if (node_in_map == NULL) {
        new_node = create_and_add_new_node(map, key);
    }

    if (node_in_map == NULL && new_node == NULL) {
        pthread_rwlock_unlock(&map->lock);
        return -1;
    }

    // if (new_node != NULL) {
    //     *created = 1;
    // } else if (node_in_map != NULL) {
    //     *created = 0;
    // }

    *created = (new_node != NULL);
    *out_node = new_node ? new_node : node_in_map;
    atomic_fetch_add(&(*out_node)->ref_cnt, 1);

    pthread_rwlock_unlock(&map->lock);
    return 0;
}

/*
 * Создает ключ для записи вида "GET <host>:<port><relative_path_from_request>". 
 */
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

/**
 * Создает и возвращает через reader структуру для нового читателя конкретной записи в кэше.
 * Захватывает mutex на ноду.
 */
int add_reader_cache_node(Cache_Node* node, Cache_Reader** reader/*,int socket*/) {
    if (reader == NULL || node == NULL) {
        return -1;
    }

    *reader = malloc(sizeof(Cache_Reader));
    if (*reader == NULL) {
        return -1;
    }

    pthread_mutex_lock(&node->mutex);
    node->readers_num++;
    (*reader)->offset = 0;
    (*reader)->dead = 0;
    (*reader)->next = node->readers;
    node->readers = *reader;
    pthread_mutex_unlock(&node->mutex);
    return 0;
}

void remove_reader(Cache_Node* node, Cache_Reader** prev_ptr, Cache_Reader** target) {
    *prev_ptr = (*target)->next;
    node->readers_num--;
    if (node->readers_num == 0 && node->state == PASS) {
        node->abort_pass = 1;
    }
}

/**
 * Удаляет структуру читателя для конкретной записи в кэше.
 * Захватывает mutex на ноду.
 */
void remove_reader_cache_node(Cache_Node* node, Cache_Reader** reader) {
    if (reader == NULL || node == NULL) {
        return;
    }

    pthread_mutex_lock(&node->mutex);
    Cache_Reader **prev_ptr = &node->readers;
    while (*prev_ptr != NULL) {
        if (*prev_ptr == *reader) {
            remove_reader(node, prev_ptr, reader);
            break;
        }
        prev_ptr = &(*prev_ptr)->next;
    }

    pthread_mutex_unlock(&node->mutex);
    // с барского плеча зафришу, даже если не нашел
    free(*reader);
    *reader = NULL;
}

/**
 * Вытесняет из буфера ту часть, которая
 * уже прочитана всеми живыми читателями.
 * Mutex необходимо захватывать до вызова функции. 
 */
void trim_cache_node(Cache_Node* node) {
    if (node->state != PASS) {
        return;
    }
    size_t min = SIZE_MAX;
    Cache_Reader* reader = node->readers;
    while (reader != NULL) {
        if (!reader->dead && reader->offset < min) {
            min = reader->offset;
        }
        reader = reader->next;
    }

    if (min == SIZE_MAX) {
        return;
    }

    if (min < node->base_offset) {
        fprintf(stderr, "unexpected error in trim_cache_node()\n");
        // по-моему, такой ситуации просто не должно случиться
        // но если все же когда-то случится, то нужно
        // для этого клиента создавать отдельное новое
        // соединение с хостом, чтобы точно все передать
        return;
    }
    size_t cut = min - node->base_offset;
    if (cut > node->response.len) {
        cut = node->response.len;
        // такого тоже не должно случиться, но тут
        // есть некая страховочка, над которой я не задумывался особо
        fprintf(stderr, "another unexpected error in trim_cache_node()\n");
    }

    if (cut == 0) {
        return;
    }

    memmove(node->response.data, node->response.data + cut, node->response.len - cut);
    node->response.len -= cut;
    node->base_offset += cut;
}

size_t copy_data_ready_to_be_sent(Cache_Node* node, Cache_Reader* reader, char* buffer, size_t cap) {
    size_t start_in_response = reader->offset - node->base_offset;
    size_t ready_in_buffer = 0;
    if (start_in_response < node->response.len) {
        ready_in_buffer = node->response.len - start_in_response;
    }
    size_t ready_total = node->recv_cnt - reader->offset;
    if (ready_in_buffer > ready_total) {
        // тоже какая-то странная ситуация:
        // получается что фактически доступное в буфере
        // больше доступного по расчетам
        fprintf(stderr, "maybe?? unexpected?? error in stream_from_cache_node()\n");
        ready_in_buffer = ready_total;
    }
    if (ready_in_buffer > cap) {
        ready_in_buffer = cap;
    }

    memcpy(buffer, node->response.data + start_in_response, ready_in_buffer);
    
    return ready_in_buffer;
}

// надо до вызова этой функции проверять, не стоит ли состояние PASS
// у записи кэша, чтобы не пытаться из нее читать.
/**
 * Пересылает данные из кэша клиенту.
 * Если кэш был помечен PASS уже после того, как клиент
 * получил эту ноду, то данные читаются через кэш.
 */
int stream_from_cache_node(Cache_Node* node, Cache_Reader *reader) {
    char buffer[8192];

    while (1) {
        pthread_mutex_lock(&node->mutex);

        while (!node->error && !node->eof && reader->offset >= node->recv_cnt) {
            pthread_cond_wait(&node->cond_var, &node->mutex);
        }

        if (node->error) {
            pthread_mutex_unlock(&node->mutex);
            return -1;
        }

        if (reader->offset >= node->recv_cnt && node->eof) {
            pthread_mutex_unlock(&node->mutex);
            return 0;
        }

        if (reader->offset < node->base_offset) {
            pthread_mutex_unlock(&node->mutex);
            fprintf(stderr, "unexpected error in stream_from_cache_node()\n");
            return -1;
        }

        size_t ready_in_buffer = copy_data_ready_to_be_sent(node, reader, buffer, sizeof(buffer));

        pthread_mutex_unlock(&node->mutex);

        if (send_all(reader->socket, buffer, ready_in_buffer) != 0) {
            pthread_mutex_lock(&node->mutex);
            reader->dead = 1;
            trim_cache_node(node);
            pthread_mutex_unlock(&node->mutex);
            return -1;           
        }

        pthread_mutex_lock(&node->mutex);
        reader->offset += ready_in_buffer;
        trim_cache_node(node);
        pthread_mutex_unlock(&node->mutex);
    }
    return 0;
}