#ifndef __HTTP_UTILS_H__
#define __HTTP_UTILS_H__

// #include <stdlib.h>

#include "http_request.h"
#include "dynamic_buffer.h"

#define MAX_BUFFER_SIZE 4096

typedef enum {
    READ_HEAD,
    READ_BODY, 
    READ_DONE
} http_read_state;

typedef struct {
    http_read_state state;
    long body_remaining;
} http_reader_state;

typedef struct {
    char* data;         
    size_t len;          
    int is_header;    
} http_chunk;

http_chunk http_reader_next(int sock, http_reader_state* st,
                            char* buf, size_t cap, size_t* len_buf,
                            long content_length);

int send_all(int sock, const void* buf, size_t len);

long parse_content_length(http_request* req);

int parse_host_and_port(http_request* req, char** out_host, char** out_port);

int build_request(const http_request *req, dynbuf *out);

int connect_hots(const char* host, const char* port);

int read_and_parse_request_head(int client_sock, http_reader_state *st, char *io_buf, size_t io_cap, 
                                size_t *io_len, http_request *req_out, long *content_length_out);

int proxy_body(int from_sock, int to_sock,http_reader_state *st, char *io_buf, 
               size_t io_cap, size_t *io_len, long content_length);

long parse_content_length_from_header_line(const char *line);

int proxy_response(int upstream_sock, int client_sock);

const char* from_absolute_path(const char *target, char *tmp, size_t tmp_cap);

int proxy_response_and_maybe_cache(int upstream_sock, int client_sock, int do_cache, dynbuf *resp_acc);

#endif