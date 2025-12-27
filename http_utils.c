#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>
#include <ctype.h>

#include "http_utils.h"

const char* find_end_line(const char* buffer, size_t len) {
    if (len < 2) {
        return NULL;
    }

    for (size_t i = 0; i + 1 < len; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            return buffer + i;
        }
    }
    return NULL;
}

http_chunk make_chunk_copy(const void* src, size_t n, int is_header) {
    http_chunk c = {0};
    c.data = (char*)malloc(n + 1); 
    if (c.data == NULL) {
        return c;
    }
    memcpy(c.data, src, n);
    c.data[n] = '\0';
    c.len = n;
    c.is_header = is_header;
    return c;
}

http_chunk http_reader_next(int sock, http_reader_state* st,
                            char* buf, size_t cap, size_t* len_buf,
                            long content_length) {
    http_chunk out = {0};
    if (st == NULL || buf == NULL || len_buf == NULL || cap == 0) {
        return out;
    }

    while (1) {
        if (st->state == READ_DONE) {
            return out;
        }

        if (st->state == READ_HEAD) {
            const char* end = find_end_line(buf, *len_buf);
            if (end != NULL) {
                size_t line_len = (size_t)(end - buf) + 2; 
                out = make_chunk_copy(buf, line_len, 1);
                if (out.data == NULL) {
                    return (http_chunk){0};
                } 

                memmove(buf, buf + line_len, *len_buf - line_len);
                *len_buf -= line_len;

                if (line_len == 2) {
                    st->state = READ_BODY;
                    if (content_length >= 0) {
                        st->body_remaining = content_length;
                    } else {
                        st->body_remaining = -1;
                    }

                    if (st->body_remaining == 0) {
                        st->state = READ_DONE;
                    }
                }

                return out;
            }
        }

        if (st->state == READ_BODY) {
            if (st->body_remaining == 0) {
                st->state = READ_DONE;
                return (http_chunk){0};
            }

            if (*len_buf > 0) {
                size_t want = *len_buf;
                if (want > cap) {
                    want = cap;
                }

                if (st->body_remaining > 0 && want > (size_t)st->body_remaining) {
                    want = (size_t)st->body_remaining;
                }

                out = make_chunk_copy(buf, want, 0);
                if (out.data == NULL) {
                    return (http_chunk){0};
                }

                memmove(buf, buf + want, *len_buf - want);
                *len_buf -= want;

                if (st->body_remaining > 0) {
                    st->body_remaining -= (long)want;
                }

                return out;
            }
        }

        if (*len_buf == cap) {
            st->state = READ_DONE;
            return (http_chunk){0};
        }

        ssize_t n = recv(sock, buf + *len_buf, cap - *len_buf, 0);
        if (n <= 0) {
            if (st->state == READ_BODY && st->body_remaining == -1) {
                st->state = READ_DONE;
                return (http_chunk){0};
            }

            st->state = READ_DONE;
            return (http_chunk){0};
        }

        *len_buf += (size_t)n;
    }
}

int send_all(int sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(sock, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

long parse_content_length(http_request* req) {
    const char* cl_value = get_http_header(req, "Content-Length");
    if (cl_value == NULL) {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    long n = strtol(cl_value, &end, 10);
    if (errno != 0 || end == cl_value || n < 0) {
        return -1;
    }
    return n;
}

int parse_host_and_port(http_request* req, char** out_host, char** out_port) {
    const char* host_value = get_http_header(req, "Host");
    if (host_value == NULL) {
        return -1;
    }

    char* tmp = strdup(host_value);
    if (tmp == NULL) {
        return -1;
    } 

    char* p = tmp;
    while (*p == ' ' || *p == '\t') {
        p++;
    } 

    if (p != tmp) {
        memmove(tmp, p, strlen(p) + 1);
    } 

    char* sep = strchr(tmp, ':');
    if (sep) {
        *sep = '\0';
        sep++;
        while (*sep == ' ' || *sep == '\t') {
            sep++;
        }
        if (*sep == '\0') { 
            free(tmp); 
            return -1; 
        }

        *out_host = strdup(tmp);
        *out_port = strdup(sep);
        free(tmp);
        if (*out_host == NULL || *out_port == NULL) {
            free(*out_host); 
            free(*out_port);
            return -1;
        }
        return 0;
    }

    *out_host = strdup(tmp);
    *out_port = strdup("80");
    free(tmp);
    if (*out_host == NULL || *out_port == NULL) {
        free(*out_host); 
        free(*out_port);
        return -1;
    }
    return 0;
}


int connect_hots(const char* host, const char* port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        return -1;
    }
    
    int sock = -1;
    struct addrinfo *iter = res;
    while (iter) {
        sock = socket(iter->ai_family, iter->ai_socktype, iter->ai_protocol);
        if (sock >= 0) {
            if (connect(sock, iter->ai_addr, iter->ai_addrlen) == 0) {
                freeaddrinfo(res);
                return sock;
            }
            close(sock);
            sock = -1;
        }
        iter = iter->ai_next;
    }


    freeaddrinfo(res);
    return -1;
}

int read_and_parse_request_head(int client_sock, http_reader_state *st, char *io_buf, size_t io_cap, 
                                size_t *io_len, http_request *req_out, long *content_length_out) {
    init_http_request(req_out);
    *content_length_out = -1;

    int got_request_line = 0;

    while (1) {
        http_chunk c = http_reader_next(client_sock, st, io_buf, io_cap, io_len, -1);
        if (c.data == NULL) {
            return -1;
        }

        if (!c.is_header) {
            free(c.data);
            return -1;
        }

        if (c.len == 2 && memcmp(c.data, "\r\n", 2) == 0) {
            free(c.data);
            break;
        }

        if (!got_request_line) {
            parse_http_request_line(req_out, c.data);
            got_request_line = 1;
        } else {
            parse_http_header(req_out, c.data);
        }

        free(c.data);
    }

    *content_length_out = parse_content_length(req_out);
    return 0;
}

const char* method_to_str(http_method m) {
    extern const char* http_method_names[];
    if ((int)m < 0 || (int)m >= METHODS_NUM - 1) {
        // return "GET";
        return NULL;
    }
    return http_method_names[m];
}

const char* version_to_str(http_version v) {
    switch (v) {
        case HTTP_1_0: return "HTTP/1.0";
        case HTTP_1_1: return "HTTP/1.1";
        default:       return "HTTP/1.0";
    }
}


const char* from_absolute_path(const char *target, char *tmp, size_t tmp_cap) {
    if (target == NULL || *target == 0) {
        // return "/";
        return NULL;
    }

    if (target[0] == '/') {
        return target;
    }

    const char *p = target;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        const char *slash = strchr(p, '/');
        if (slash == NULL) {
            // return "/";
            return NULL;
        }
        return slash;
    }

    const char *slash = strchr(p, '/');
    if (slash != NULL) {
        return slash;
    }

    tmp = NULL; 
    tmp_cap = 0;
    // return "/";
    return NULL;
}

int build_request(const http_request *req, dynbuf *out) {
    if (req == NULL || out == NULL) {
        return -1;
    }

    out->data = NULL;
    out->len = 0;
    out->cap = 0;

    char tmp[1024];
    char absolute_tmp[4096];

    const char *method = method_to_str(req->method);
    if (method == NULL) {
        return -1;
    }
    const char *ver = version_to_str(req->version);

    const char *path = from_absolute_path(req->target_path, absolute_tmp, sizeof(absolute_tmp));
    if (path == NULL) {
        return -1;
    }

    int n = snprintf(tmp, sizeof(tmp), "%s %s %s\r\n", method, path, ver);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        free_dynbuf(out);
        return -1;
    }
    if (add_dynbuf(out, tmp, (size_t)n) != 0) {
        free_dynbuf(out);
        return -1;
    }

    http_header *h;
    STAILQ_FOREACH(h, &req->headers, entries) {
        if (!h->key || !h->value) {
            continue;
        }

        if (strcasecmp(h->key, "Proxy-Connection") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "Proxy-Authenticate") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "Proxy-Authorization") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "Connection") == 0) {
            continue;
        }

        if (strcasecmp(h->key, "Keep-Alive") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "TE") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "Trailer") == 0) {
            continue;
        }
        if (strcasecmp(h->key, "Upgrade") == 0) {
            continue;
        }

        size_t need = strlen(h->key) + 2 + strlen(h->value) + 2 + 1;
        char *line = (char*)malloc(need);
        if (line == NULL) { 
            free_dynbuf(out); 
            return -1; 
        }

        snprintf(line, need, "%s: %s\r\n", h->key, h->value);

        int rc = dynbuf_append_str(out, line);
        free(line);
        if (rc != 0) { 
            free_dynbuf(out); 
            return -1; 
        }
    }

    if (dynbuf_append_str(out, "Connection: close\r\n") != 0) {
        free_dynbuf(out);
        return -1;
    }

    if (dynbuf_append_str(out, "\r\n") != 0) {
        free_dynbuf(out);
        return -1;
    }

    return 0;
}


int proxy_body(int from_sock, int to_sock,http_reader_state *st, char *io_buf, 
               size_t io_cap, size_t *io_len, long content_length) {
    while (1) {
        http_chunk c = http_reader_next(from_sock, st, io_buf, io_cap, io_len, content_length);
        if (c.data == NULL) {
            break;
        }

        if (send_all(to_sock, c.data, c.len) != 0) {
            free(c.data);
            return -1;
        }
        free(c.data);
    }
    return 0;
}

int proxy_response_and_maybe_cache(int upstream_sock, int client_sock, int do_cache, dynbuf *resp_acc) {
    char buf[8192];

    while (1) {
        ssize_t n = recv(upstream_sock, buf, sizeof(buf), 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            return -1;
        }

        if (do_cache) {
            if (add_dynbuf(resp_acc, buf, (size_t)n) != 0) {
                do_cache = 0;
            }
        }

        if (send_all(client_sock, buf, (size_t)n) != 0) {
            return -1;
        }
    }
}



long parse_content_length_from_header_line(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    const char *key = "Content-Length:";
    size_t klen = strlen(key);
    if (strncasecmp(p, key, klen) != 0) {
        return -2; 
    }

    p += klen;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    errno = 0;
    char *end = NULL;
    long n = strtol(p, &end, 10);
    if (errno != 0 || end == p || n < 0) {
        return -1;
    }
    return n; // НЕ ЗАБЫТЬ УБРАТЬ коммент
    // return -1;
}


int proxy_response(int upstream_sock, int client_sock) {
    http_reader_state st = {.state = READ_HEAD, .body_remaining = -2};
    char io_buf[MAX_BUFFER_SIZE];
    size_t io_len = 0;

    long content_length = -1;

    while (1) {
        http_chunk c = http_reader_next(upstream_sock, &st, io_buf, sizeof(io_buf), &io_len, -1);
        if (c.data == NULL) {
            return -1;
        } 

        if (!c.is_header) { 
            free(c.data); 
            return -1; 
        }

        if (content_length < 0) {
            long v = parse_content_length_from_header_line(c.data);
            if (v >= 0) {
                content_length = v;
            } else if (v == -1) {
                content_length = -1;
            }
        }

        if (send_all(client_sock, c.data, c.len) != 0) { 
            free(c.data); 
            return -1; 
        }

        if (c.len == 2 && memcmp(c.data, "\r\n", 2) == 0) {
            free(c.data);
            break;
        }
        free(c.data);
    }

    while (1) {
        http_chunk c = http_reader_next(upstream_sock, &st, io_buf, sizeof(io_buf), &io_len, content_length);
        if (c.data == NULL) {
            break;
        }

        if (send_all(client_sock, c.data, c.len) != 0) { 
            free(c.data); 
            return -1; 
        }
        free(c.data);
    }

    return 0;
}