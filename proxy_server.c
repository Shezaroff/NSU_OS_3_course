#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <limits.h>

#include "http_request.h"
#include "dynamic_buffer.h"
#include "http_utils.h"
#include "cache_map.h"
#include "cleanup_thread.h"
#include "host_receiver_thread.h"

#define INITIALIZATION_ERROR -1
#define INVALID_SERVER_PORT -1
#define INVALID_ARGUMENT -2
#define NO_EMPTY_NODE -1

#define NEED_502 2
#define JUST_CLEANUP 1

#define REQUEST_QUEUE_SIZE 32
#define MAX_THREADS 550

static Cache_Map cache;

/**
 * Описывает аргументы для потока-обработчика.
 */
typedef struct client_args {
    sem_t* server_threads_sem;
    int socket;
} client_args;

/**
 * Отправляет клиенту ошибку на все случаи жизни.
 */
static void send_simple_502(int client_sock) {
    const char* resp =
        "HTTP/1.0 502 Bad Gateway\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    (void)send_all(client_sock, resp, strlen(resp));
}

/**
 * Аккуратно пытается закрыть сокет.
 */
void safe_socket_close(int* socket) {
    if (*socket >= 0) {
        close(*socket);
        *socket = -1;
    }
}

/**
 * Читает запрос от клиента. Достает хост и порт. 
 * Строит запрос в байтовом виде для целевого сервера.
 */
int read_request_and_build(int client_sock, http_reader_state* st,
                           char* io_buf, size_t io_cap, size_t* io_len,
                           http_request** req_out, int* req_cl_out,
                           char** host_out, char** port_out,
                           dynbuf* built_raw_req_out) {
    http_request* req = NULL;
    char* host = NULL;
    char* port = NULL;
    long req_cl = -1;
    
    alloc_http_request(&req);
    if (req == NULL) {
        return -1;
    }

    if (read_and_parse_request_head(client_sock, st, io_buf, io_cap, io_len, req, &req_cl) == -1) {
        free_http_request(&req);
        return -1;
    } 

    if (parse_host_and_port(req, &host, &port) == -1) {
        free_http_request(&req);
        return -1;
    }

    if (build_request(req, built_raw_req_out) != 0) {
        free_http_request(&req);
        free(host);
        free(port);
        return -1;
    }

    *req_out = req;
    *req_cl_out = req_cl;
    *host_out = host;
    *port_out = port;
    return 0;
}

/**
 * Проверяет, нужно ли передавать тело запроса.
 * Если нет Content-Length, то в случае POST вызывает ошибку,
 * а в ином случае не передает тело.
 */
int should_transfer_request_body(const http_request* req, int req_cl) {
    if (req_cl > 0) {
        return 1;
    }
    if (req_cl == 0) {
        return 0;
    }
    if (req->method == POST) {
        return -1;
    }
    return 0;
}

/**
 * Освобождает всякие выделенные при помощи malloc сущности, а убирает ссылку на запись в кэше.
 */
void free_info_before_return(Cache_Node* node, receiver_args* recv_args) {
    pthread_mutex_lock(&node->mutex);
    node->error = 1;
    pthread_cond_broadcast(&node->cond_var);
    pthread_mutex_unlock(&node->mutex);

    atomic_fetch_sub(&node->recv_cnt, 1);
    free(recv_args->host);
    free(recv_args->port);
    free(recv_args->req_data);
    free(recv_args);
    return;
}

/**
 * Запускает поток, получающий данные от целевого сервера.
 */
int start_recv_thread(Cache_Node* node, char* host, char* port,
                      dynbuf* built_raw_req) {
    receiver_args* recv_args = malloc(sizeof(receiver_args));
    if (recv_args == NULL) {
        pthread_mutex_lock(&node->mutex);
        node->error = 1;
        pthread_cond_broadcast(&node->cond_var);
        pthread_mutex_unlock(&node->mutex);
        atomic_fetch_sub(&node->ref_cnt, 1);
        return -1;
    }

    recv_args->cache_node = node;
    atomic_fetch_add(&node->ref_cnt, 1);

    recv_args->host = strdup(host);
    recv_args->port = strdup(port);
    recv_args->req_data = malloc(built_raw_req->len);

    if (recv_args->host == NULL || recv_args->port == NULL || recv_args->req_data == NULL) {
        free_info_before_return(node, recv_args);
        return -1;
    }

    memcpy(recv_args->req_data, built_raw_req->data, built_raw_req->len);
    recv_args->req_len = built_raw_req->len;

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, reciever_thread, recv_args) != 0) {
        free_info_before_return(node, recv_args);
        return -1;
    }
    pthread_detach(recv_thread);

    return 0;
}

/**
 * Ищет запись в кэше. 
 * Возвращает:
 * 1 - ответ получен из кэша
 * 0 - запрос не кэшируется
 * В случае, если запись найдена и при этом 
 * не помечена PASS, передает данные клиенту. 
 * Если записи нет, то запускает поток для получения ответа
 * от целевого сервера. 
 */
int try_cache_request(int client_sock, http_request* req,
                   int req_cl, char* host, char* port,
                   dynbuf* built_raw_req) {
    if ((req->method != GET) || (req_cl > 0)) {
        return 0;
    }

    char cache_key[2048];
    if (build_cache_key(cache_key, sizeof(cache_key), host, port, req) != 0) {
        return 0;
    }

    Cache_Node* node = NULL;
    int created = 0;

    if (get_set_cache_map(&cache, cache_key, &node, &created) == -1) {
        return 0;
    }

    pthread_mutex_lock(&node->mutex);
    cache_node_state st_node = node->state;
    size_t base_off = node->base_offset;
    pthread_mutex_unlock(&node->mutex);

    if (!created && st_node == PASS) {
        atomic_fetch_sub(&(node->ref_cnt), 1);
        return 0;
    }

    if (!created && base_off > 0) {
        atomic_fetch_sub(&(node->ref_cnt), 1);
        return 0;
    }

    if (created) {
        if (start_recv_thread(node, host, port, built_raw_req) == -1) {
            return 0;
        }
    }

    Cache_Reader* reader = NULL;
    if (add_reader_cache_node(node, &reader) == -1) {
        pthread_mutex_lock(&node->mutex);
        if (node->readers_num > 0) {
            node->readers_num--;
        }
        pthread_mutex_unlock(&node->mutex);

        atomic_fetch_sub(&(node->ref_cnt), 1);
        return 0;
    }

    reader->socket = client_sock;

    stream_from_cache_node(node, reader);

    remove_reader_cache_node(node, &reader);

    atomic_fetch_sub(&(node->ref_cnt), 1);
    return 1;
}

/**
 * Читает запрос от клиента.
 * Пытается найти данные в кэше и переслать их клиенту.
 * В случае если сделать это не удалось по какой-то причине,
 * то подключается напрямую к целевому серверу в обход кэш мапы
 * и передает данные клиенту.
 */
int process(int* client_sock, int* host_sock,
            http_request** req, char** host, char** port,
            dynbuf* built_raw_req) {
    http_reader_state st = {.state = READ_HEAD, .body_remaining = -2};
    char io_buf[MAX_BUFFER_SIZE];
    size_t io_len = 0;

    int req_cl = -1;

    if (read_request_and_build(*client_sock, &st, io_buf, sizeof(io_buf),
                               &io_len, req, &req_cl, host, port, 
                               built_raw_req) == -1) {
        return NEED_502;
    }
     
    int rc = try_cache_request(*client_sock, *req, req_cl, *host, *port, built_raw_req);
    if (rc == 1) {
        return JUST_CLEANUP;
    }

    *host_sock = connect_host(*host, *port);
    if (*host_sock < 0) {
        return NEED_502;
    }
    
    if (send_all(*host_sock, built_raw_req->data, built_raw_req->len) == -1) {
        return NEED_502;
    }

    int need_req_body = should_transfer_request_body(*req, req_cl);
    if (need_req_body == -1) {
        return NEED_502;
    } 
    if (need_req_body == 1) {
        if (proxy_body(*client_sock, *host_sock, &st, io_buf, sizeof(io_buf), &io_len, req_cl) == -1) {
            return NEED_502;
        }
    }

    proxy_response(*host_sock, *client_sock);
    return JUST_CLEANUP;
}

/**
 * Потоковая функция обработки клиента.
 * Обрабатывает запрос, отчищает все использованное.
 */
void* handle_client(void* vargs) {
    client_args* args = (client_args*)vargs;

    int client_sock = args->socket;
    sem_t* server_threads_sem = args->server_threads_sem;
    free(args);

    int host_sock = -1;
    http_request* req = NULL;
    char* host = NULL;
    char* port = NULL;
    dynbuf built_raw_req = {0};

    int rc = process(&client_sock, &host_sock, &req, &host, &port, &built_raw_req);
    if (rc == NEED_502 && client_sock >= 0) {
        send_simple_502(client_sock);
    }

    safe_socket_close(&client_sock);
    safe_socket_close(&host_sock);

    free(host);
    free(port);

    free_dynbuf(&built_raw_req);
    if (req != NULL) {
        free_http_request(&req);
    }

    sem_post(server_threads_sem);
    return NULL;
}

/**
 * Инициализирует прокси сервер.
 */
int init_proxy_server(int* server_socket, int server_port, int requests_queue_size) {
    *server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (*server_socket == -1) {
        perror("error create socket");
        return INITIALIZATION_ERROR;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    server_addr.sin_port = htons(server_port);

    int opt = 1;
    if (setsockopt(*server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    if (bind(*server_socket, (struct sockaddr*) &server_addr, sizeof(server_addr)) == -1) {
        perror("error socket bind");
        close(*server_socket);
        *server_socket = -1;
        return INITIALIZATION_ERROR;
    }

    if (listen(*server_socket, requests_queue_size) == -1) {
        perror("error listen socket");
        close(*server_socket);
        *server_socket = -1;
        return INITIALIZATION_ERROR;
    }

    printf("Server was started...\n");
    return 0;
}

int parse_port(const char* env_port) {
    char* endptr;
    long port;

    if (env_port == NULL || *env_port == '\0') {
        return INVALID_SERVER_PORT;
    }

    errno = 0;
    port = strtol(env_port, &endptr, 10);

    if (errno != 0 || *endptr != '\0' ||
        port < 1 || port > 65535) {
        return INVALID_SERVER_PORT;
    }

    return (int)port;
}

/**
 * Потоковая функция самого сервера.
 */
void* proxy_server(void* args) {
    int server_socket;
    char* env_port = getenv("PROXY_PORT");
    if (env_port == NULL) {
        printf("ERRRRORORORORO\n");
        exit(EXIT_FAILURE);
    }
    // printf("%s\n", env_port);
    int port = parse_port(env_port);
    if (port == INVALID_SERVER_PORT) {
        port = 5423;
        printf("Invalid PROXY_PORT value, port set to default (5423)");
    }

    if (init_proxy_server(&server_socket, port, REQUEST_QUEUE_SIZE) == INITIALIZATION_ERROR) {
        printf("Server was not initted");
        return NULL;
    } 

    sem_t server_threads_sem;
    sem_init(&server_threads_sem, 0, MAX_THREADS);

    int client_socket;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t client_tid;
    client_args* cl_arg;

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr*) &client_addr, &addr_len);
        if (client_socket == -1) {
            perror("error accept socket");
            continue;
        }
        
        sem_wait(&server_threads_sem);

        // Пока пусть просто будет маллок, а то я уже задушился
        // делать что-то адекватное. Потом переделаю, если что
        cl_arg = malloc(sizeof(client_args));
        if (cl_arg == NULL) {
            perror("error client_args malloc");
            return NULL;
        }

        cl_arg->server_threads_sem = &server_threads_sem;
        cl_arg->socket = client_socket;
        
        if (pthread_create(&client_tid, &attr, handle_client, cl_arg) == -1) {
            perror("pthread_create() error");
            free(cl_arg);
            close(client_socket);
            sem_post(&server_threads_sem);
        }
    }

    close(server_socket);
    pthread_attr_destroy(&attr);
    sem_destroy(&server_threads_sem);
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    init_cache_map(&cache);
    pthread_t cleaner_tid;
    cache_cleaner_args* ca = malloc(sizeof(*ca));
    if (!ca) {
        perror("error malloc cache_cleaner_args");
    } else {
        ca->map = &cache;
        ca->interval_sec = 5;                 
        ca->max_size_bytes = MAX_SIZE_CACHE_MAP;
        ca->percent_for_del = 50;

        if (pthread_create(&cleaner_tid, NULL, cache_cleaner_thread, ca) != 0) {
            perror("error creating cache cleaner thread");
            free(ca);
        } else {
            pthread_detach(cleaner_tid);
        }
    }

    pthread_t server_thread;

    if (pthread_create(&server_thread, NULL, proxy_server, NULL) != 0) {
        perror("error creating server thread");
        exit(EXIT_FAILURE);
    }

    pthread_join(server_thread, NULL);

    destroy_cache_map(&cache);

    return 0;
}

// curl -L --http1.0   -x http://127.0.0.1:5423   -o debian1.iso   http://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-13.2.0-amd64-netinst.iso
// curl -L --http1.0   -x http://127.0.0.1:5423   -o debian4.iso   http://cdimage.debian.org/debian-cd/current/arm64/iso-cd/debian-13.2.0-arm64-netinst.iso
// curl -L --http1.0   -x http://127.0.0.1:5423   -o /dev/null http://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-13.3.0-amd64-netinst.iso