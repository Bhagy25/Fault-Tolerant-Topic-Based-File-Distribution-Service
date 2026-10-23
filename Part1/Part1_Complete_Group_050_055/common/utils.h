#ifndef IT305_UTILS_H
#define IT305_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "protocol.h"

typedef struct {
    int fd;
    unsigned char buffer[IT305_CHUNK_SIZE];
    size_t position;
    size_t available;
} SocketReader;

/* Send all bytes, even when send() performs a partial write.
 * bytes_sent (if non-NULL) is updated for bytes accepted by the kernel,
 * including bytes accepted before a subsequent error. */
int send_all(int fd, const void *buffer, size_t length, uint64_t *bytes_sent);
void reader_init(SocketReader *reader, int fd);
/* 1: line read; 0: EOF at line boundary; -1: error / premature EOF */
int reader_read_line(SocketReader *reader, char *line, size_t capacity);
/* Read exactly 'length' binary bytes; never consume bytes past the payload. */
int reader_read_file(SocketReader *reader, FILE *output, uint64_t length,
                     uint64_t *bytes_received);
int validate_port(const char *text, uint16_t *port);
int mkdir_p(const char *path);
double monotonic_seconds(void);
int safe_basename(const char *name);
int join_path(char *dest, size_t capacity, const char *first, const char *second);

#endif
