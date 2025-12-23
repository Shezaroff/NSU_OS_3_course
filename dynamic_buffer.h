#ifndef __DYNAMIC_BUFFER_H__
#define __DYNAMIC_BUFFER_H__

#include <stdlib.h>

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} dynbuf;

int dynbuf_append_str(dynbuf *buffer, const char *s);

int add_dynbuf(dynbuf* buffer, const void* src, size_t n);

void free_dynbuf(dynbuf* buffer);

#endif