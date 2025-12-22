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

#define INITIALIZATION_ERROR -1
#define INVALID_SERVER_PORT -1
#define INVALID_ARGUMENT -2
#define NO_EMPTY_NODE -1


#define REQUEST_QUEUE_SIZE 32
#define MAX_THREADS 1024

typedef struct client_args {
    sem_t* server_threads_sem;
    int socket;
} client_args;

void handle_client(void* args) {

}

int init_proxy_server(int* server_socket, int server_port, int requests_queue_size) {
    *server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (*server_socket == -1) {
        perror("error create socket");
        return INITIALIZATION_ERROR;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(INADDR_ANY); //Возможно, неверно
    server_addr.sin_port = htons(server_port);

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
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
}

int parse_port(const char *env_port) {
    char *endptr;
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

void run_proxy_server(void* args) {
    int server_socket;
    int port;
    char *env_port = getenv("PROXY_PORT");
    if (port = parse_port(env_port) == INVALID_SERVER_PORT) {
        port = 5423;
        printf("Invalid PROXY_PORT value, port set to default (5423)");
    }

    if (init_proxy_server(&server_socket, port, REQUEST_QUEUE_SIZE) == INITIALIZATION_ERROR) {
        printf("Server was not initted");
        return;
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
            return;
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
    sem_destroy(&sem_destroy);
}

int main() {
    
}