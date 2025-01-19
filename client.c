/**
 * nonstop_networking
 * CS 341 - Spring 2024
 */
#include "format.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "common.h"

#define BUF_SIZE 2048
#define MAX_HEADER_LEN 1024

char **parse_args(int argc, char **argv);
verb check_args(char **args);

static struct addrinfo *result = NULL;
static int server_socket = -1;
static int local_fd = -1;
static char **args;

static char buf[BUF_SIZE];
static int buf_offset, buf_len;

static int end_client;

void clean_up() {
    // Free args
    free(args);

    // Free resources
    freeaddrinfo(result);
    result = NULL;

    // Close server connection
    if (server_socket != -1) {
        close(server_socket);
    }

    // Close opened file
    if (local_fd != -1) {
        close(local_fd);
    }
}

void signal_handler(int signum) {
    end_client = 1;
}

int connect_to_server(const char *host, const char *port) {
    int sfd, s;
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_STREAM; 
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        LOG("getaddrinfo: %s", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    sfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    if (connect(sfd, result->ai_addr, result->ai_addrlen) == -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);
    result = NULL;
    return sfd;
}

int parse_response_header() {
    // Read from server
    while (read_from_socket(server_socket, buf, &buf_offset, &buf_len, BUF_SIZE) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
    }

    int token_len;
    for (token_len = 0; buf[token_len] != '\n' && token_len < buf_len; token_len++);
    if (token_len >= MAX_HEADER_LEN) {
        // Header exceed maximum size of header and does not end with '\n' (Bad request)
        print_invalid_response();
        return -1;
    }
    buf[token_len] = '\0';
    // Update buffer offset
    buf_offset = token_len + 1;

    if (strcmp(buf, "OK") == 0) {
        LOG("Get server response: OK");
    } else if (strcmp(buf, "ERROR") == 0) {
        LOG("Get server response: ERROR");
        // Parse error message
        for (token_len = buf_offset; buf[token_len] != '\n' && token_len < buf_len; token_len++);
        if (token_len >= MAX_HEADER_LEN) {
            // Header exceed maximum size of header and does not end with '\n' (Bad request)
            print_invalid_response();
            return -1;
        }
        buf[token_len] = '\0';
        // Print error message
        print_error_message(&buf[buf_offset]);
        // Update buffer offset
        buf_offset += token_len + 1;
        return 1;
    } else {
        print_invalid_response();
        return -1;
    }

    return 0;
}

int get(const char *remote, const char *local) {
    // Send request
    char token[1024];
    sprintf(token, "GET %s\n", remote);
    if (write_all(server_socket, token, strlen(token)) == -1) {
        return -1;
    }

    // Close the write end
    shutdown(server_socket, SHUT_WR);

    int res = parse_response_header();
    if (res != 0) {
        return -1;
    }

    // Open local file
    local_fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
    if (local_fd == -1) {
        LOG("open(): Cannot open file %s", local);
        return -1;
    }

    // Read data size
    size_t data_size = * (size_t *) &buf[buf_offset];
    LOG("data_size = %ld", data_size);

    // Update buffer offset
    buf_offset += sizeof(size_t);
    assert(buf_offset <= buf_len);

    // Get data form socket
    ssize_t total = 0;
    while (recv_data(server_socket, local_fd, buf, &buf_offset, &buf_len, BUF_SIZE, &total) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
    }
    LOG("write %ld bytes to local file", total);
    
    // Check data size
    if (total < (ssize_t) data_size) {
        print_too_little_data();
        return -1;
    } else if (total > (ssize_t) data_size) {
        print_received_too_much_data();
        return -1;
    }

    return 0;
}

int put(const char *remote, const char *local) {
    // Open local file
    local_fd = open(local, O_RDONLY);
    if (local_fd == -1) {
        LOG("open(): File %s does not exist", local);
        return -1;
    }

    // Send request
    char token[1024];
    sprintf(token, "PUT %s\n", remote);
    if (write_all(server_socket, token, strlen(token)) == -1) {
        return -1;
    }

    // Get data size
    struct stat file_stat;
    if (fstat(local_fd, &file_stat) == -1) {
        perror("fstat");
        return -1;
    }
    size_t data_size = file_stat.st_size;
    LOG("data_size = %ld", data_size);

    // Send data size to server
    if (write_all(server_socket,  (char *) &data_size, sizeof(size_t)) == -1) {
        return -1;
    }

    // Send data to server
    ssize_t total = 0;
    while (send_data(server_socket, local_fd, buf, &buf_offset, &buf_len, BUF_SIZE, &total) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
    }

    // Close the write end
    shutdown(server_socket, SHUT_WR);

    // Read response
    int res = parse_response_header();
    if (res != 0) {
        LOG("Failed to parse response header");
        return -1;
    } else {
        print_success();
    }

    return 0;
}

int delete(const char *remote) {
    // Send request
    char token[1024];
    sprintf(token, "DELETE %s\n", remote);
    if (write_all(server_socket, token, strlen(token)) == -1) {
        return -1;
    }

    // Close the write end
    shutdown(server_socket, SHUT_WR);

    // Read response
    int res = parse_response_header();
    if (res != 0) {
        return -1;
    } else {
        print_success();
    }

    return 0;
}

int list() {
    // Send request
    char token[1024];
    sprintf(token, "LIST\n");
    if (write_all(server_socket, token, strlen(token)) == -1) {
        return -1;
    }

    // Close the write end
    shutdown(server_socket, SHUT_WR);

    // Read response
    int res = parse_response_header();
    if (res != 0) {
        return -1;
    }

    // Read data size
    size_t data_size = * (size_t *) &buf[buf_offset];

    // Update buffer offset
    buf_offset += sizeof(size_t);
    assert(buf_offset <= buf_len);

    // Get data form socket
    ssize_t total = 0;
    while (recv_data(server_socket, 1, buf, &buf_offset, &buf_len, BUF_SIZE, &total) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
    }
    LOG("write %ld bytes to local file", total);
    
    // Check data size
    if (total < (ssize_t) data_size) {
        print_too_little_data();
        return -1;
    } else if (total > (ssize_t) data_size) {
        print_received_too_much_data();
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    /* Set up handlers */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); // Simply ignore SIGPIPE

    /* Parse arguments */
    verb v = check_args(argv);
    args = parse_args(argc, argv);
    char *host = args[0], *port = args[1];
    char *remote = NULL, *local = NULL;
    if (args[3] != NULL) {
        remote = args[3];
        if (args[4] != NULL) {
            local = args[4];
        }
    }

    /* Connect to server */
    server_socket = connect_to_server(host, port);

    /* Perform VERB */
    switch (v) {
        case GET:
            get(remote, local);
            break;
        case PUT:
            put(remote, local);
            break;
        case DELETE:
            delete(remote);
            break;
        case LIST:
            list();
            break;
        default:
            exit(EXIT_FAILURE);
    }

    /* Clean up and exit */
    clean_up();
    LOG("Closing client...");
    return 0;
}

/**
 * Given commandline argc and argv, parses argv.
 *
 * argc argc from main()
 * argv argv from main()
 *
 * Returns char* array in form of {host, port, method, remote, local, NULL}
 * where `method` is ALL CAPS
 */
char **parse_args(int argc, char **argv) {
    if (argc < 3) {
        return NULL;
    }

    char *host = strtok(argv[1], ":");
    char *port = strtok(NULL, ":");
    if (port == NULL) {
        return NULL;
    }

    char **args = calloc(1, 6 * sizeof(char *));
    args[0] = host;
    args[1] = port;
    args[2] = argv[2];
    char *temp = args[2];
    while (*temp) {
        *temp = toupper((unsigned char)*temp);
        temp++;
    }
    if (argc > 3) {
        args[3] = argv[3];
    }
    if (argc > 4) {
        args[4] = argv[4];
    }

    return args;
}

/**
 * Validates args to program.  If `args` are not valid, help information for the
 * program is printed.
 *
 * args     arguments to parse
 *
 * Returns a verb which corresponds to the request method
 */
verb check_args(char **args) {
    if (args == NULL) {
        print_client_usage();
        exit(1);
    }

    char *command = args[2];

    if (strcmp(command, "LIST") == 0) {
        return LIST;
    }

    if (strcmp(command, "GET") == 0) {
        if (args[3] != NULL && args[4] != NULL) {
            return GET;
        }
        print_client_help();
        exit(1);
    }

    if (strcmp(command, "DELETE") == 0) {
        if (args[3] != NULL) {
            return DELETE;
        }
        print_client_help();
        exit(1);
    }

    if (strcmp(command, "PUT") == 0) {
        if (args[3] == NULL || args[4] == NULL) {
            print_client_help();
            exit(1);
        }
        return PUT;
    }

    // Not a valid Method
    print_client_help();
    exit(1);
}
