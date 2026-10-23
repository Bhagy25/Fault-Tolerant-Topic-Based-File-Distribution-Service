#define _POSIX_C_SOURCE 200809L
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

int send_all(int fd, const void *buffer, size_t length, uint64_t *bytes_sent) {
    const unsigned char *current = buffer;
    size_t position = 0;
    while (position < length) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, current + position, length - position, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, current + position, length - position, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EPIPE; return -1; }
        position += (size_t)n;
        if (bytes_sent != NULL) *bytes_sent += (uint64_t)n;
    }
    return 0;
}

void reader_init(SocketReader *reader, int fd) {
    reader->fd = fd;
    reader->position = 0;
    reader->available = 0;
}

static int reader_fill(SocketReader *reader) {
    for (;;) {
        ssize_t n = recv(reader->fd, reader->buffer, sizeof(reader->buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return (int)n;
        reader->position = 0;
        reader->available = (size_t)n;
        return 1;
    }
}

int reader_read_line(SocketReader *reader, char *line, size_t capacity) {
    size_t used = 0;
    if (capacity < 2) { errno = EINVAL; return -1; }
    for (;;) {
        if (reader->position == reader->available) {
            int rc = reader_fill(reader);
            if (rc <= 0) {
                if (rc == 0 && used > 0) errno = EPROTO;
                return (rc == 0 && used == 0) ? 0 : -1;
            }
        }
        unsigned char c = reader->buffer[reader->position++];
        if (c == '\n') { line[used] = '\0'; return 1; }
        if (c == '\0' || c == '\r' || used + 1 >= capacity) {
            errno = EPROTO;
            return -1;
        }
        line[used++] = (char)c;
    }
}

int reader_read_file(SocketReader *reader, FILE *output, uint64_t length,
                     uint64_t *bytes_received) {
    while (length > 0) {
        if (reader->position == reader->available) {
            int rc = reader_fill(reader);
            if (rc <= 0) {
                if (rc == 0) errno = EPROTO;
                return -1;
            }
        }
        size_t available = reader->available - reader->position;
        size_t take = length < (uint64_t)available ? (size_t)length : available;
        size_t written = fwrite(reader->buffer + reader->position, 1, take, output);
        if (written != take) { errno = EIO; return -1; }
        reader->position += take;
        length -= (uint64_t)take;
        if (bytes_received != NULL) *bytes_received += (uint64_t)take;
    }
    return 0;
}

int validate_port(const char *text, uint16_t *port) {
    char *end;
    errno = 0;
    if (text == NULL || *text == '\0' || *text == '-') return -1;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > 65535) return -1;
    *port = (uint16_t)value;
    return 0;
}

int mkdir_p(const char *path) {
    size_t length = strlen(path);
    if (length == 0 || length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char temp[PATH_MAX];
    memcpy(temp, path, length + 1);
    for (char *p = temp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1;
            struct stat st;
            if (stat(temp, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
            *p = '/';
        }
    }
    if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (stat(temp, &st) != 0 || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

int safe_basename(const char *name) {
    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; ++c) {
        if (*c == '/' || *c == '\\' || *c == '\n' || *c == '\r') return 0;
    }
    return 1;
}

int join_path(char *dest, size_t capacity, const char *first, const char *second) {
    int n = snprintf(dest, capacity, "%s/%s", first, second);
    if (n < 0 || (size_t)n >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
