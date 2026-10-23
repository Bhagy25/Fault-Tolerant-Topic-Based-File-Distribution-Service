#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result;
    int status = getaddrinfo(host, port, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "[client] getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *candidate = result; candidate != NULL;
         candidate = candidate->ai_next) {
        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static int parse_count(const char *line, uint64_t *count) {
    if (strncmp(line, "OK ", 3) != 0) return -1;
    const char *value = line + 3;
    if (*value < '0' || *value > '9') return -1;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *count = (uint64_t)parsed;
    return 0;
}

static int parse_file_header(char *line, char **filename, uint64_t *size) {
    if (strncmp(line, "FILE ", 5) != 0) return -1;
    char *start = line + 5;
    char *last_space = strrchr(start, ' ');
    if (last_space == NULL) return -1;
    *last_space = '\0';
    if (!safe_basename(start) || last_space[1] < '0' || last_space[1] > '9') return -1;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(last_space + 1, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *filename = start;
    *size = (uint64_t)parsed;
    return 0;
}

typedef struct ReceivedPath {
    char path[PATH_MAX];
    struct ReceivedPath *next;
} ReceivedPath;

static int remember_path(ReceivedPath **head, const char *path) {
    ReceivedPath *node = malloc(sizeof *node);
    if (!node) return -1;
    (void)snprintf(node->path, sizeof node->path, "%s", path);
    node->next = *head;
    *head = node;
    return 0;
}

static void release_paths(ReceivedPath **head, int unlink_files) {
    while (*head) {
        ReceivedPath *node = *head;
        *head = node->next;
        if (unlink_files) (void)unlink(node->path);
        free(node);
    }
}

/* Returns 0 on completed transfer, 1 on unexpected TCP disconnect, -1 on
 * irrecoverable protocol / disk / server response error. */
static int download_attempt(int fd, const char *topic, const char *destination,
                            uint64_t *attempt_bytes, uint64_t *attempt_files,
                            ReceivedPath **written_paths) {
    char request[IT305_MAX_LINE];
    int request_length = snprintf(request, sizeof request, "GET %s\n", topic);
    if (request_length < 0 || (size_t)request_length >= sizeof request) return -1;
    if (send_all(fd, request, (size_t)request_length, NULL) != 0) return 1;
    SocketReader reader;
    reader_init(&reader, fd);
    char header[IT305_MAX_LINE];
    int rc = reader_read_line(&reader, header, sizeof header);
    if (rc != 1) return (errno == EPROTO && rc < 0) ? 1 : 1;
    if (strncmp(header, "ERR ", 4) == 0) {
        fprintf(stderr, "[client] Server rejected request: %s\n", header);
        return -1;
    }
    uint64_t expected_files = 0;
    if (parse_count(header, &expected_files) != 0) return -1;
    printf("[client] topic=%s expected_files=%" PRIu64 "\n", topic, expected_files);
    for (;;) {
        rc = reader_read_line(&reader, header, sizeof header);
        if (rc != 1) return 1;
        if (strcmp(header, "DONE") == 0) return *attempt_files == expected_files ? 0 : -1;
        if (*attempt_files >= expected_files) return -1;
        char *filename;
        uint64_t filesize;
        if (parse_file_header(header, &filename, &filesize) != 0) return -1;
        char path[PATH_MAX];
        if (join_path(path, sizeof path, destination, filename) != 0) return -1;
        int out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (out_fd < 0) { perror("[client] open output"); return -1; }
        if (remember_path(written_paths, path) != 0) {
            close(out_fd);
            (void)unlink(path);
            return -1;
        }
        FILE *output = fdopen(out_fd, "wb");
        if (!output) { close(out_fd); (void)unlink(path); return -1; }
        int received_ok = reader_read_file(&reader, output, filesize, attempt_bytes);
        if (fclose(output) != 0) received_ok = -2;
        if (received_ok != 0) {
            (void)unlink(path);
            if (received_ok == -2) return -1;
            return 1;
        }
        ++*attempt_files;
    }
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir> [max_retries]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port;
    if (validate_port(argv[2], &port) != 0) return EXIT_FAILURE;
    const char *topic = argv[3];
    if (!safe_basename(topic) || strchr(topic, '.') || strlen(topic) >= IT305_MAX_TOPIC)
        return EXIT_FAILURE;
    int max_retries = 10;
    if (argc == 6) {
        char *end;
        errno = 0;
        long value = strtol(argv[5], &end, 10);
        if (errno || *end || end == argv[5] || value < 0 || value > 1000) return EXIT_FAILURE;
        max_retries = (int)value;
    }
    char destination[PATH_MAX];
    if (join_path(destination, sizeof destination, argv[4], topic) != 0 ||
        mkdir_p(destination) != 0) return EXIT_FAILURE;
    uint64_t transmitted = 0, useful = 0, file_count = 0;
    int retries = 0;
    double started = monotonic_seconds();
    int success = 0;
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        ReceivedPath *written_paths = NULL;
        int fd = connect_to_server(argv[1], argv[2]);
        uint64_t attempt_bytes = 0, attempt_files = 0;
        int rc = 1;
        if (fd >= 0) {
            rc = download_attempt(fd, topic, destination, &attempt_bytes, &attempt_files,
                                  &written_paths);
            close(fd);
        }
        transmitted += attempt_bytes;
        if (rc == 0) {
            release_paths(&written_paths, 0);
            useful = attempt_bytes;
            file_count = attempt_files;
            success = 1;
            break;
        }
        release_paths(&written_paths, 1);
        if (rc < 0) break;
        if (attempt == max_retries) break;
        ++retries;
        fprintf(stderr, "[client] connection lost, restarting topic from beginning (retry %d/%d)\n",
                retries, max_retries);
        /* Capped exponential backoff; avoid infinite retry loops. */
        unsigned delay = (unsigned)(1u << (retries <= 3 ? retries - 1 : 2));
        sleep(delay);
    }
    double seconds = monotonic_seconds() - started;
    uint64_t redundant = transmitted >= useful ? transmitted - useful : 0;
    printf("METRIC mode=case1 status=%s files=%" PRIu64 " received_bytes=%" PRIu64
           " useful_bytes=%" PRIu64 " redundant_bytes=%" PRIu64
           " retries=%d seconds=%.6f MBps=%.3f\n",
           success ? "OK" : "ERROR", file_count, transmitted, useful, redundant,
           retries, seconds, seconds > 0 ? (double)useful / seconds / 1000000.0 : 0.0);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
