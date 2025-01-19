/**
 * nonstop_networking
 * CS 341 - Spring 2024
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>

#include "format.h"
#include "common.h"
#include "includes/vector.h"
#include "includes/dictionary.h"
#include "includes/set.h"

#define BACKLOG 128
#define MAX_EVENTS 128
#define BUF_SIZE 2048
#define MAX_HEADER_LEN 1024

static int server_socket, epoll_fd;
static struct addrinfo *result = NULL;
static char *temp_dir;
static set *local_files;
static dictionary *clients_infos;
static int stop_server;

typedef enum {
    P_HEADER, P_VERB, P_DONE
} status_t;

typedef enum {
    OK, E_AGAIN, E_BAD_HEADER, E_BAD_FILESIZE, E_NONEXIST
} err_t;

typedef struct {
    // Essential metadata for resuming process
    status_t status;
    verb v;
    int client_fd;

    // Buffer
    char buf[BUF_SIZE];
    int buf_len;
    int buf_offset;

    // Client request
    int local_fd;
    char *file_name;
    size_t data_size;
    ssize_t byte_processed;

    // Error type
    err_t err;
} client_info;

client_info *create_client_info(int client_fd) {
    client_info *info = (client_info *) malloc(sizeof(client_info));

    info->status = P_HEADER;
    info->v = V_UNKNOWN;
    info->client_fd = client_fd;

    info->buf_len = 0;
    info->buf_offset = 0;

    info->local_fd = 0;
    info->file_name = NULL;
    info->data_size = 0;
    info->byte_processed = 0;

    info->err = OK;

    return info;
}

void close_client(client_info *info) {
    // Remove from dictionary
    dictionary_remove(clients_infos, &info->client_fd);
    // Free resources allocated by client
    free(info->file_name);
    close(info->client_fd);
    close(info->local_fd);
    free(info);
}

void clean_up() {
    /* Resource in setup*/
    freeaddrinfo(result);
    close(server_socket);
    close(epoll_fd);

    // Close all files
    vector *files = set_elements(local_files);
    VECTOR_FOR_EACH(files, file, {
        char file_path[1024];
        sprintf(file_path, "%s/%s", temp_dir, (char *) file);
        unlink(file_path);
    });
    vector_destroy(files);

    // Close dir
    rmdir(temp_dir);

    /* Global data structure */
    set_destroy(local_files);

    vector *infos = dictionary_values(clients_infos);
    VECTOR_FOR_EACH(infos, info, {
        close_client(info);
    });
    vector_destroy(infos);
    dictionary_destroy(clients_infos);
}

void signal_handler(int signum) {
    if (signum == SIGINT) {
        stop_server = 1;
    }
}

/**
 * Make a connection and listen on server socket.
 * Exit the program on failure.
 */
void connection_init(char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *result;
    int s = getaddrinfo(NULL, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    server_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (server_socket == -1) {
        perror(NULL);
        freeaddrinfo(result);
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    if (bind(server_socket, result->ai_addr, result->ai_addrlen) == -1) {
        perror(NULL);
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(result);

    if (listen(server_socket, BACKLOG) == -1) {
        perror(NULL);
        close(server_socket);
        exit(EXIT_FAILURE);
    }
}

int get(client_info *info) {
    if (info->data_size == 0) {
        if (!set_contains(local_files, info->file_name)) {
            info->err = E_NONEXIST;
            return -1;
        }

        // Open file
        char file_path[1024];
        sprintf(file_path, "%s/%s", temp_dir, info->file_name);
        int local_fd = open(file_path, O_RDONLY);
        if (local_fd == -1) {
            info->err = E_NONEXIST;
            return -1;
        }
        info->local_fd = local_fd;

        // Get data size
        struct stat file_stat;
        if (fstat(info->local_fd, &file_stat) == -1) {
            perror("fstat");
            return -1;
        }
        info->data_size = file_stat.st_size;
        LOG("data_size = %ld", info->data_size);

        // Response
        if (write_all(info->client_fd, "OK\n", 3) == -1) {
            // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
            return -1;
        }

        // Send data size to client
        if (write_all(info->client_fd, (char *) &info->data_size, sizeof(size_t)) == -1) {
            // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
            return -1;
        }

        // Modify epoll type to EPOLLOUT
        struct epoll_event event = {0};
        event.events = EPOLLOUT;
        event.data.fd = info->client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, info->client_fd, &event) == -1) {
            perror("epoll_ctl");
            return -1;
        }
    }

    // Write data to client
    if (send_data(info->client_fd, info->local_fd, info->buf, &info->buf_offset, &info->buf_len, BUF_SIZE, &info->byte_processed) == -1) {
        if (errno == EAGAIN) {
            info->err = E_AGAIN;
        }
        return -1;
    }

    assert((size_t) info->byte_processed == info->data_size);
    return 0;
}

int put(client_info *info) {
    if (info->data_size == 0) {
        // Open file
        char file_path[1024];
        sprintf(file_path, "%s/%s", temp_dir, info->file_name);
        int local_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
        info->local_fd = local_fd;

        // Add to set
        LOG("Try to add file %s to local file set", info->file_name);
        set_add(local_files, info->file_name);

        // Read data size
        size_t data_size = * (size_t *) &info->buf[info->buf_offset];
        // Update buffer offset
        info->buf_offset += sizeof(size_t);
        assert(info->buf_offset <= info->buf_len);
        info->data_size = data_size;
    }

    // Read data and write to local file  
    if (recv_data(info->client_fd, info->local_fd, info->buf, &info->buf_offset, &info->buf_len, BUF_SIZE, &info->byte_processed) == -1) {
        if (errno == EAGAIN) {
            info->err = E_AGAIN;
        }
        return -1;
    }
    LOG("write %ld bytes to local file", info->byte_processed);

    // Check data size
    if (info->byte_processed != (ssize_t) info->data_size) {
        info->err = E_BAD_FILESIZE;
        return -1;
    }
    
    // Response
    if (write_all(info->client_fd, "OK\n", 3) == -1) {
        // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
        return -1;
    }

    return 0;
}

int delete(client_info *info) {
    if (!set_contains(local_files, info->file_name)) {
        info->err = E_NONEXIST;
        return -1;
    }

    // Get file path
    char file_path[1024];
    sprintf(file_path, "%s/%s", temp_dir, info->file_name);

    // Delete file
    unlink(file_path);

    // Remove file from file set
    set_remove(local_files, info->file_name);

    // Response
    if (write_all(info->client_fd, "OK\n", 3) == -1) {
        // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
        return -1;
    }

    return 0;
}

int list(client_info *info) {
    size_t data_size = 0;

    if (set_cardinality(local_files) != 0) {
        vector *elements = set_elements(local_files);
        VECTOR_FOR_EACH(elements, file_name, {
            data_size += strlen(file_name) + 1;
        });
        data_size--; // No '\n' in the end
        vector_destroy(elements);

        info->data_size = data_size;
    }
    LOG("data_size = %ld", info->data_size);

    // Response
    if (write_all(info->client_fd, "OK\n", 3) == -1) {
        // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
        return -1;
    }

    // Send data size to client
    if (write_all(info->client_fd, (char *) &info->data_size, sizeof(size_t)) == -1) {
        LOG("list: Fail to write data size to client");
        // Error: 1. SIGPIPE 2. SIGINT
        return -1;
    }

    // Send data to client
    if (info->data_size == 0) {
        return 0;
    }

    int i = 0;
    char *file_name;
    char msg[1024];
    vector *elements = set_elements(local_files);
    for (; i < (int) set_cardinality(local_files) - 1; i++) {
        file_name = vector_get(elements, i);
        sprintf(msg, "%s\n", file_name);
        if (write_all(info->client_fd, msg, strlen(msg)) == -1) {
            // Error: 1. SIGPIPE 2. SIGINT
            perror(NULL);
            vector_destroy(elements);
            return -1;
        }
    }
    file_name = vector_get(elements, i);
    if (write_all(info->client_fd, file_name, strlen(file_name)) == -1) {
        // Error: 1. SIGPIPE 2. SIGINT
        vector_destroy(elements);
        return -1;
    }
    vector_destroy(elements);
    return 0;
}

int parse_request_header(client_info *info) {
    int token_len;
    for (token_len = 0; info->buf[token_len] != '\n' && token_len < info->buf_len; token_len++);
    if (token_len >= MAX_HEADER_LEN) {
        // Header exceed maximum size of header and does not end with '\n' (Bad request)
        info->err = E_BAD_HEADER;
        return -1;
    }
    
    info->buf[token_len] = '\0';
    char *verb = strtok(info->buf, " ");
    char *file_name = strtok(NULL, " ");
    LOG("Header: verb = %s, filename = %s", verb, file_name);

    // Update buffer offset
    info->buf_offset = token_len + 1;

    if (strcmp(verb, "GET") == 0) {
        info->v = GET;

        if (!file_name) {
            info->err = E_BAD_HEADER;
            return -1;
        }
        info->file_name = strdup(file_name);
    } else if (strcmp(verb, "PUT") == 0) {
        info->v = PUT;

        if (!file_name) {
            info->err = E_BAD_HEADER;
            return -1;
        }
        info->file_name = strdup(file_name);
    } else if (strcmp(verb, "DELETE") == 0) {
        info->v = DELETE;

        if (!file_name) {
            info->err = E_BAD_HEADER;
            return -1;
        }
        info->file_name = strdup(file_name);
    } else if (strcmp(verb, "LIST") == 0) {
        info->v = LIST;
    } else {
        // Invalid verb
        info->err = E_BAD_HEADER;
        return -1;
    }

    return 0;
}

int process_client_request(client_info *info) {
    // Process header
    if (info->status == P_HEADER) {
        if (read_from_socket(info->client_fd, info->buf, &info->buf_offset, &info->buf_len, BUF_SIZE) == -1) {
            // Possible error: 1. no enough data (EAGAIN) 2. SIGINT
            if (errno == EAGAIN) {
                LOG("No enough data");
                info->err = E_AGAIN;
            }
            return -1;
        }

        // Parse header
        if (parse_request_header(info) == -1) {
            // Possible error: 1. E_BAD_HEADER 2. E_NONEXIST 2. SIGINT
            return -1;
        }

        // Update status
        info->status = P_VERB;
    }

    // Perform request
    if (info->status == P_VERB) {
        switch (info->v) {
            case GET:
                if (get(info) == -1) {
                    // Possible errors: 1. Connection abort 3. SIGINT
                    return -1;
                }
                break;
            case PUT:
                if (put(info) == -1) {
                    // Possible errors: 1. No enough data 2. Connection abort 3. SIGINT
                    return -1;
                }
                break;
            case DELETE:
                if (delete(info) == -1) {
                    // Possible errors: 1. nonexisted file 2. connection abort 3. SIGINT
                    return -1;
                }
                break;
            case LIST:
                if (list(info) == -1) {
                    // Possible errors: 1. nonexisted file 2. connection abort 3. SIGINT
                    return -1;
                }
                break;
            case V_UNKNOWN:
                info->err = E_BAD_HEADER;
                return -1;
        }

        // Update status
        info->status = P_DONE;
    }

    return 0;
}

int send_error_msg(const char *msg, client_info *info) {
    if (write_all(info->client_fd, "ERROR\n", 6) == -1) {
        // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
        return -1;
    }

    if (write_all(info->client_fd, msg, strlen(msg)) == -1) {
        // Possible errors: 1. connection abort (EPIPE) 2. SIGINT
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_server_usage();
        exit(EXIT_FAILURE);
    }

    struct epoll_event event = {0}, events[MAX_EVENTS] = {0};
    local_files = string_set_create();
    clients_infos = int_to_shallow_dictionary_create();

    // Set signal handler
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); // Simply ignore SIGPIPE

    // Make connection
    connection_init(argv[1]);

    // Epoll setup
    epoll_fd = epoll_create(1);
    if (epoll_fd == -1) {
        perror("epoll");
        exit(EXIT_FAILURE);
    }
    // Add socket to interest list
    event.events = EPOLLIN;
    event.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    // Create temporary directory 
    char template[7] = "XXXXXX";
    temp_dir = mkdtemp(template);
    print_temp_directory(temp_dir);  

    // Process events
    while (!stop_server) {
        int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (event_count == -1) {
            perror("epoll_wait");
            break;
        }

        // LOG("server wake up with %d new event", event_count);

        for (int i = 0; i < event_count; i++) {
            if (events[i].data.fd == server_socket) {
                // New client
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                int client_fd = accept(server_socket, (struct sockaddr *) &client_addr, &client_addr_len);
                if (client_fd == -1) {
                    perror("accept");
                    continue;
                }
                LOG("New connection accepted: client_fd = %d", client_fd);

                // Set client_fd as nonblocking
                int flags = fcntl(client_fd, F_GETFL, 0);
                if (flags == -1) {
                    perror("fcntl");
                    break;
                }
                if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                    perror("fcntl");
                    break;
                }

                // Add client socket to epoll instance
                memset(&event, 0, sizeof(struct epoll_event));
                event.events = EPOLLIN;
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                    perror("epoll_ctl");
                    break;
                }

                // Add client info to dictionary
                dictionary_set(clients_infos, &client_fd, create_client_info(client_fd));
            } else {
                // Client request
                client_info *info = dictionary_get(clients_infos, &events[i].data.fd);
                info->err = OK;
                // LOG("Process client_fd %d", info->client_fd);
                process_client_request(info);

                // If there is an error, send error message
                switch (info->err) {
                    case E_BAD_HEADER:
                        print_invalid_response();
                        send_error_msg(err_bad_request, info);
                        break;
                    case E_BAD_FILESIZE:
                        send_error_msg( err_bad_file_size, info);
                        break;
                    case E_NONEXIST:
                        send_error_msg(err_no_such_file, info);
                        break;
                    case E_AGAIN:
                        // Leave the client and execute until there is available data
                        break;
                    case OK:
                        // No action
                        break;
                }

                // If client request is finished or has error, remove client_fd from interest list
                if (info->err != E_AGAIN) {
                    // If PUT fail, delete the local file
                    if (info->v == PUT && info->err != OK) {
                        set_remove(local_files, info->file_name);
                        unlink(info->file_name);
                    }

                    dictionary_remove(clients_infos, &info->client_fd);

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, info->client_fd, &events[i]) == -1) {
                        perror("epoll_ctl");
                        break;
                    }

                    close_client(info);
                }
            }
        }
    }

    LOG("Closing server...");

    clean_up();
    
    return 0;
}
