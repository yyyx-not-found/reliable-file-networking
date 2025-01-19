/**
 * nonstop_networking
 * CS 341 - Spring 2024
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/select.h>
#include <fcntl.h>

#include "common.h"

ssize_t read_all(int fd, char *buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t res = read(fd, buffer + total, count - total);
        if (res == 0) {
            return total;
        } else if (res > 0) {
            total += res;
        } else if (res == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return total;
}

ssize_t write_all(int fd, const char *buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t res = write(fd, buffer + total, count - total);
        if (res == 0) {
            return total;
        } else if (res > 0) {
            total += res;
        } else if (res == -1 && errno == EINTR) {
            continue;
        } else if (res == -1 && errno == EPIPE) {
            return 0;
        } else {
            return -1;
        }
    }
    return total;
}

ssize_t read_from_socket(int sfd, char *buf, int *buf_offset, int *buf_len, size_t buf_size) {
    // Flush buffer if possible
    if (*buf_offset == *buf_len) {
        *buf_offset = 0;
        *buf_len = 0;
    }

    ssize_t nread = recv(sfd, &buf[*buf_len], buf_size - *buf_len, 0);
    // LOG("Read %ld bytes from socket. Buffer info: offset = %d, len = %d", nread, *buf_offset, *buf_len);
    if (nread == -1) {
        // 1. EAGAIN 2. SIGINT
        return -1;
    }

    if (nread == 0) {
        // Connection closed
        // LOG("read_from_socket: socket closed");
        return 0;
    }

    // Update buffer len
    *buf_len += nread;

    if ((size_t) nread < buf_size - *buf_len) {
        errno = EAGAIN;
        return -1;
    }

    assert(*buf_offset <= *buf_len);
    assert(*buf_len <= (int) buf_size);

    return nread;
}

ssize_t send_data(int sfd, int local_fd, char *buf, int *buf_offset, int *buf_len, size_t buf_size, ssize_t *byte_write) {
    while (1) {
        // Send data to socket
        while (*buf_offset < *buf_len) {
            ssize_t tmp = send(sfd, &buf[*buf_offset], *buf_len - *buf_offset, 0);
            if (tmp == -1) {
                // 1. EAGAIN 2. SIGINT
                return -1;
            }
            // Update buffer offset
            *buf_offset += tmp;
            // Update byte_write
            *byte_write += tmp;
        }

        // Flush buffer
        assert(*buf_offset == *buf_len);
        *buf_offset = 0;
        *buf_len = 0;

        // Read more data
        ssize_t res = read(local_fd, &buf[*buf_len], buf_size - *buf_len);
        if (res == -1) {
            // Error: SIGINT
            perror(NULL);
            return -1;
        }

        // Update buffer len
        *buf_len += res;
        assert(*buf_len <= (int) buf_size); 

        if (*buf_len == 0) {
            break;
        }       
    }
    LOG("send %ld bytes", *byte_write);
    return 0;
}

ssize_t recv_data(int sfd, int local_fd, char *buf, int *buf_offset, int *buf_len, size_t buf_size, ssize_t *byte_read) {
    while (1) {
        ssize_t res = write(local_fd, &buf[*buf_offset], *buf_len - *buf_offset);
        if (res == -1) {
            // Error: SIGINT
            perror(NULL);
            return -1;
        }
        // Update buffer offset
        *buf_offset += res;
        *byte_read += res;
        // assert(*buf_offset == *buf_len);
        // Read more data from client
        if (read_from_socket(sfd, buf, buf_offset, buf_len, buf_size) == -1) {
            // 1. EAGAIN 2. SIGINT
            return -1;
        }
        // If no more data, end the process
        if (*buf_len == 0) {
            break;
        }
    }
    return 0;
}
