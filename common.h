/**
 * nonstop_networking
 * CS 341 - Spring 2024
 */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define LOG(...)                      \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n");        \
    } while (0);

typedef enum { GET, PUT, DELETE, LIST, V_UNKNOWN } verb;

/**
 * Attempts to read all count bytes from fd into buffer.
 * Assumes buffer is large enough.
 *
 * Returns number of read bytes on success, or -1 on failure.
 * On failure, errno is set and should be handled.
 */
ssize_t read_all(int socket, char *buffer, size_t count);

/**
 * Attempts to write all count bytes from buffer to fd.
 * Assumes buffer contains at least count bytes.
 *
 * Returns number of written bytes on success, or -1 on failure.
 * On failure, errno is set and should be handled.
 * Failures include:
 * 1. EPIPE: Remote side is closed.
 */
ssize_t write_all(int socket, const char *buffer, size_t count);

/**
 * Attempts to read data form socket to fill the buffer.
 *
 * Returns number of bytes read from socket on success, or -1 on failure.
 * On failure, errno is set and should be handled.
 * Failures include:
 * 1. EAGAIN: No enough data.
 */
ssize_t read_from_socket(int sfd, char *buf, int *buf_offset, int *buf_len, size_t buf_size);

/**
 * Attempts to write all data from lcoal file to sfd.
 *
 * Returns 0 on success, or -1 on failure.
 * On failure, errno is set and should be handled.
 * Failures include:
 * 1. EPIPE: Remote side is closed.
 * 2. EAGAIN: Buffer is full.
 */
ssize_t send_data(int sfd, int local_fd, char *buf, int *buf_offset, int *buf_len, size_t buf_size, ssize_t *byte_write);

/**
 * Attempts to write all data from sfd to local file.
 *
 * Returns 0 on success, or -1 on failure.
 * On failure, errno is set and should be handled.
 * Failures include:
 * 1. EPIPE: Remote side is closed.
 * 2. EAGAIN: No enough data.
 */
ssize_t recv_data(int sfd, int local_fd, char *buf, int *buf_offset, int *buf_len, size_t buf_size, ssize_t *byte_read);