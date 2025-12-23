#include "dynamic_buffer.h"

#include <string.h>

#define DEFAULT_SIZE 4096

int dynbuf_append_str(dynbuf *buffer, const char *s) {
    return add_dynbuf(buffer, s, strlen(s));
}

int add_dynbuf(dynbuf* buffer, const void* src, size_t n) {
    if (n == 0) {
        return 0;
    }

    if (buffer->len + n > buffer->cap) {
        
        size_t new_cap;

        if (buffer->cap != 0) {
            new_cap = buffer->cap;
        } else {
            new_cap = DEFAULT_SIZE;
        }

        while (new_cap < buffer->len + n) {
            new_cap *= 2;
        }

        char* p = realloc(buffer->data, new_cap);
        if (!p) {
            return -1;
        }
        buffer->data = p;
        buffer->cap = new_cap;
    }

    memcpy(buffer->data + buffer->len, src, n);
    buffer->len += n;
    return 0;
}

void free_dynbuf(dynbuf* buffer) {
    if (buffer == NULL) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = buffer->cap = 0;
}
