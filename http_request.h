#ifndef __HTTP_METHODS_H__
#define __HTTP_METHODS_H__

#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

typedef enum {
    GET,
    HEAD,
    // OPTIONS,
    // TRACE,
    // DELETE,
    // PUT,
    POST,
    // PATCH,
    NOT_IMPLEMENTED,
    METHODS_NUM
} http_method;

typedef enum {
    HTTP_1_0,
    HTTP_1_1,
    NOT_SUPPORTED
} http_version;

typedef struct http_header {
    char* key;
    char* value;
    STAILQ_ENTRY(http_header) entries;
} http_header;

typedef STAILQ_HEAD(http_header_queue, http_header) http_header_queue;

typedef struct http_request {
    http_method method;
    char* target_path;
    http_version version;
    http_header_queue headers;
} http_request;

void alloc_http_request(http_request** request);

void free_http_request(http_request** req);

int add_http_header(http_request *request, const char *key, const char *value);

void parse_http_request_line(http_request* result, const char* line);

void parse_http_header(http_request* result, const char* line);

#endif