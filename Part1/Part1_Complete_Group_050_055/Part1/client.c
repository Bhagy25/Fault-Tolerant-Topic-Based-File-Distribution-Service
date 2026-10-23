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

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir>\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port;
    if (validate_port(argv[2], &port) != 0) {
        fprintf(stderr, "[client] Invalid port\n"); return EXIT_FAILURE;
    }
    const char *topic = argv[3];
    if (!safe_basename(topic) || strchr(topic, '.') != NULL || strlen(topic) >= IT305_MAX_TOPIC) {
        fprintf(stderr, "[client] Invalid topic\n"); return EXIT_FAILURE;
    }
    char destination[PATH_MAX];
    if (join_path(destination, sizeof destination, argv[4], topic) != 0 ||
        mkdir_p(destination) != 0) {
        perror("[client] creating destination"); return EXIT_FAILURE;
    }
    int fd = connect_to_server(argv[1], argv[2]);
    if (fd < 0) { perror("[client] connect"); return EXIT_FAILURE; }
    double started = monotonic_seconds();
    int outcome = EXIT_FAILURE;
    uint64_t bytes_received = 0, files_received = 0, expected_files = 0;
    char request[IT305_MAX_LINE];
    int request_length = snprintf(request, sizeof request, "GET %s\n", topic);
    if (request_length < 0 || (size_t)request_length >= sizeof request ||
        send_all(fd, request, (size_t)request_length, NULL) != 0) {
        perror("[client] sending request"); goto done;
    }

    SocketReader reader;
    reader_init(&reader, fd);
    char header[IT305_MAX_LINE];
    if (reader_read_line(&reader, header, sizeof header) != 1) {
        perror("[client] reading server acknowledgment"); goto done;
    }
    if (strncmp(header, "ERR ", 4) == 0) {
        fprintf(stderr, "[client] Server rejected request: %s\n", header);
        goto done;
    }
    if (parse_count(header, &expected_files) != 0) {
        fprintf(stderr, "[client] Invalid acknowledgment: %s\n", header);
        goto done;
    }
    printf("[client] topic=%s expected_files=%" PRIu64 "\n", topic, expected_files);

    for (;;) {
        if (reader_read_line(&reader, header, sizeof header) != 1) {
            perror("[client] connection dropped before DONE"); goto done;
        }
        if (strcmp(header, "DONE") == 0) {
            if (files_received != expected_files) {
                fprintf(stderr, "[client] Count mismatch: expected=%" PRIu64
                        " received=%" PRIu64 "\n", expected_files, files_received);
                goto done;
            }
            outcome = EXIT_SUCCESS;
            break;
        }
        if (files_received >= expected_files) {
            fprintf(stderr, "[client] Server sent too many files\n"); goto done;
        }
        char *filename;
        uint64_t filesize;
        if (parse_file_header(header, &filename, &filesize) != 0) {
            fprintf(stderr, "[client] Invalid FILE header\n"); goto done;
        }
        char path[PATH_MAX];
        if (join_path(path, sizeof path, destination, filename) != 0) {
            perror("[client] destination file path"); goto done;
        }
        /* Never follow pre-existing symlinks in the download directory. */
        int out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (out_fd < 0) { perror("[client] opening output file"); goto done; }
        FILE *output = fdopen(out_fd, "wb");
        if (output == NULL) {
            perror("[client] fdopen");
            close(out_fd);
            goto done;
        }
        int received_ok = reader_read_file(&reader, output, filesize, &bytes_received);
        if (fclose(output) != 0) received_ok = -1;
        if (received_ok != 0) {
            perror("[client] receiving file");
            (void)unlink(path); /* Never leave the current partial file as complete. */
            goto done;
        }
        files_received++;
    }

done:
    {
        double seconds = monotonic_seconds() - started;
        double mbps = seconds > 0 ? (double)bytes_received / seconds / 1000000.0 : 0;
        printf("METRIC mode=part1 status=%s files=%" PRIu64 " bytes=%" PRIu64
               " seconds=%.6f MBps=%.3f\n", outcome == EXIT_SUCCESS ? "OK" : "ERROR",
               files_received, bytes_received, seconds, mbps);
    }
    close(fd);
    return outcome;
}
