#include "host_receiver_thread.h"
#include "http_request.h"
#include "http_utils.h"

void add_cache_node(Cache_Node* node, const void* data, size_t n) {
    add_dynbuf(&node->response, data, n);
    node->recv_cnt += n;
}

void maybe_change_to_pass(Cache_Node* node) {
    if (node->state != PASS) {
        node->state = PASS;
    }
    if (node->readers_num == 0) {
        node->abort_pass = 1;
    }
}

void* reciever_thread(void* arg) {
    receiver_args* a = arg;
    Cache_Node* node = a->cache_node;
    int ok = 1;

    int sock = connect_hots(a->host, a->port);
    if (ok && sock < 0) {
        pthread_mutex_lock(&node->mutex);
        node->error = 1;
        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);
        ok = 0;
    }

    if (ok && send_all(sock, a->req_data, a->req_len) != 0) {
        pthread_mutex_lock(&node->mutex);
        node->error = 1;
        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);
        close(sock);
        ok = 0;;
    }

    http_reader_state st = {.state = READ_HEAD, .body_remaining = -2};
    char io_buf[MAX_BUFFER_SIZE];
    size_t io_len = 0;
    long content_length = -1;

    while (ok && 1) {
        http_chunk chunk = http_reader_next(sock, &st, io_buf, sizeof(io_buf), &io_len, -1);
        if (!chunk.data) { 
            pthread_mutex_lock(&node->mutex);
            node->error = 1;
            pthread_cond_broadcast(&node->cond_var);
            pthread_mutex_unlock(&node->mutex);
            close(sock);
            ok = 0;
            break;
        }
        // return NULL;

        if (content_length < 0) {
            long maybe_cl = parse_content_length_from_header_line(chunk.data);
            if (maybe_cl >= 0) {
                content_length = maybe_cl;
            }
        }

        pthread_mutex_lock(&node->mutex);

        if (node->abort_pass) {
            pthread_mutex_unlock(&node->mutex);
            free(chunk.data);
            close(sock);
            ok = 0;
            break;
        }

        add_cache_node(node, chunk.data, chunk.len);

        if (content_length > (long)MAX_SIZE_CACHE_NODE) {
            maybe_change_to_pass(node);
        }

        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);

        if (chunk.len == 2 && memcmp(chunk.data, "\r\n", 2) == 0) {
            free(chunk.data);
            break;
        }
        free(chunk.data);
    }

    while (ok && 1) {
        http_chunk chunk = http_reader_next(sock, &st, io_buf, sizeof(io_buf), &io_len, content_length);
        if (!chunk.data) {
            break;
        }

        pthread_mutex_lock(&node->mutex);

        if (node->abort_pass) {
            pthread_mutex_unlock(&node->mutex);
            free(chunk.data);
            close(sock);
            ok = 0;
            break;
        }

        add_cache_node(node, chunk.data, chunk.len);

        if (node->recv_cnt > MAX_SIZE_CACHE_NODE) {
            maybe_change_to_pass(node);
        }

        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);

        free(chunk.data);
    }

    if (ok) {
        close(sock);
    
        pthread_mutex_lock(&node->mutex);
        node->eof = 1;
        if (node->state != PASS) {
            node->state = DONE;
        }
        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);
    } else {
        pthread_mutex_lock(&node->mutex);
        node->state = PASS;
        if (!node->response_freed) {
            free_dynbuf(&node->response);
            node->recv_cnt = 0;
            node->base_offset = 0;
            node->response_freed = 1;
        }
        node->eof = 1;
        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);
    }

    free(a->host);
    free(a->port);
    free(a->req_data);
    free(a);

    return NULL;
}
