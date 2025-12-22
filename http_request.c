#include "http_request.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*static */const char* http_method_names[] = {
    "GET",
    "HEAD",
    "POST"
};

void alloc_http_request(http_request** request) {
    *request = malloc(sizeof(http_request));
    if (*request == NULL) {
        return;
    }

    (*request)->method = NOT_IMPLEMENTED;
    (*request)->target_path = NULL;
    (*request)->version = NOT_SUPPORTED;
    STAILQ_INIT(&(*request)->headers);
}

void free_http_request(http_request** req) {
    if (req == NULL || *req == NULL) {
        return;
    }
    http_header* header;
    http_request* request = *req;
    while ((header = STAILQ_FIRST(&request->headers)) != NULL) {
        STAILQ_REMOVE_HEAD(&request->headers, entries);
        if (header->key != NULL) {
            free(header->key);
        }
        if (header->value != NULL) {
            free(header->value);
        }
        if (header != NULL) {
            free(header);
        }
    }
    free(request->target_path);
    request->target_path = NULL;
    free(request);
}

int add_http_header(http_request *request, const char *key, const char *value)
{
    http_header *header = (http_header *)malloc(sizeof(*header));
    if (!header) return 0;

    header->key = strdup(key);
    header->value = strdup(value);
    if (!header->key || !header->value) {
        free(header->key);
        free(header->value);
        free(header);
        return 0;
    }

    STAILQ_INSERT_TAIL(&request->headers, header, entries);
    return 1;
}

void init_invalid_http_request(http_request* result) {
    result->method = NOT_IMPLEMENTED;
    result->version = NOT_SUPPORTED;
    result->target_path = NULL;
}

void parse_http_request_line(http_request* result, const char* line) {
    const char *end = strstr(line, "\r\n");
    size_t n;
    if (end != NULL) {
        n = (size_t)(end - line);
    } else {
        // Ну вообще это странно, что нет конца строки
        // n = strlen(line);
    }

    char *request_line = strndup(line, n);
    if (request_line == NULL) {
        init_invalid_http_request(result);
        return;
    }

    char method[16];
    char url[4096];
    char version[16];

    int got = sscanf(request_line, "%15s %4095s %15s", method, url, version);
    free(request_line);

    if (got != 3) {
        init_invalid_http_request(result);
        return;
    }

    int found = 0;
    for (int i = 0; i < METHODS_NUM; i++) {
        if (strcmp(method, http_method_names[i]) == 0) {
            result->method = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        init_invalid_http_request(result);
        return;
    }

    result->target_path = strdup(url);

    if (strcmp(version, "HTTP/1.0") == 0) {
        result->version = HTTP_1_0;
    } else if (strcmp(version, "HTTP/1.1") == 0) {
        result->version = HTTP_1_1;
    } else {
        result->version = NOT_SUPPORTED;
    }
    // Тут надо еще прикинуть, нужно ли возвращать полностью неверный запрос
    // или сохранять этот в кэше
}

void trim_right_space(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t')) {
        str[len] = '\0';
        len--;
    }
}

void parse_http_header(http_request* result, const char* line) {
    const char *end = strstr(line, "\r\n");
    size_t n;
    if (end != NULL) {
        n = (size_t)(end - line);
    } else {
        // Ну вообще это странно, что нет конца строки
        // n = strlen(line);
    }

    char *hdr_line = strndup(line, n);
    if (hdr_line == NULL) {
        return;
    }
    
    char *sep = strchr(hdr_line, ':');
    if (!sep) {
        free(hdr_line);
        return; 
    }

    *sep = '\0';
    char *key_part = hdr_line;
    char *val_part = sep + 1;

    while (*val_part == ' ' || *val_part == '\t') {
        val_part++;
    }
    trim_right_space(key_part);
    trim_right_space(val_part);

    add_http_header(result, key_part, val_part);
    free(hdr_line);
}

int main() {
    http_request* request;
    alloc_http_request(&request);

    char* line = "GET / HTTP/1.1\r\n";
    char* line2 = "Host: www.example.re\r\n";
    char* line3 = "Range: bytes=0-511\r\n";

    parse_http_request_line(request, line);
    parse_http_header(request, line2);
    parse_http_header(request, line3);
    printf("%s\n", request->target_path);
    http_header *hdr;
    STAILQ_FOREACH(hdr, &request->headers, entries) {
        printf("%s: %s\n", hdr->key, hdr->value);
    }
    free_http_request(&request);
}