#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "topics.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* The topic map is loaded once, before accepting any clients. It is then
 * immutable, so concurrent worker threads can safely share it. */
static TopicMap topic_map;
static volatile sig_atomic_t stopping = 0;
static int listen_fd = -1;

typedef struct {
    int client_fd;
} ClientArgs;

static void stop_server(int signo) {
    (void)signo;
    stopping = 1;
    if (listen_fd >= 0) close(listen_fd);
}

static int send_line(int fd, const char *line) {
    return send_all(fd, line, strlen(line), NULL);
}

static int send_file(int fd, const TopicEntry *topic, const TopicFile *file,
                     uint64_t *file_bytes_sent) {
    char path[PATH_MAX];
    if (join_path(path, sizeof path, topic->dir_path, file->name) != 0) return -1;

    /* O_NOFOLLOW prevents a file swapped for a symlink from escaping the root. */
    int source = open(path, O_RDONLY | O_NOFOLLOW);
    if (source < 0) return -1;
    struct stat st;
    if (fstat(source, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size != file->size) {
        close(source);
        errno = ESTALE;
        return -1;
    }

    char header[IT305_MAX_LINE];
    int len = snprintf(header, sizeof header, "FILE %s %" PRIu64 "\n", file->name, file->size);
    if (len < 0 || (size_t)len >= sizeof header || send_line(fd, header) != 0) {
        close(source);
        return -1;
    }

    unsigned char buffer[IT305_CHUNK_SIZE];
    uint64_t remaining = file->size;
    while (remaining > 0) {
        size_t want = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
        ssize_t n = read(source, buffer, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = EIO;
            close(source);
            return -1;
        }
        if (send_all(fd, buffer, (size_t)n, file_bytes_sent) != 0) {
            close(source);
            return -1;
        }
        remaining -= (uint64_t)n;
    }
    return close(source);
}

static void handle_client(int fd) {
    double start = monotonic_seconds();
    uint64_t bytes_sent = 0;
    size_t files_sent = 0;
    int success = 0;
    SocketReader reader;
    reader_init(&reader, fd);
    char request[IT305_MAX_LINE];
    int rc = reader_read_line(&reader, request, sizeof request);
    if (rc != 1) goto done;

    if (strncmp(request, "GET ", 4) != 0 || request[4] == '\0') {
        (void)send_line(fd, "ERR INVALID_REQUEST\n");
        goto done;
    }
    const char *requested_topic = request + 4;
    const TopicEntry *topic = topic_map_find(&topic_map, requested_topic);
    if (topic == NULL) {
        (void)send_line(fd, "ERR UNKNOWN_TOPIC\n");
        goto done;
    }

    TopicFileList files;
    if (topic_list_files(topic, &files) != 0) {
        perror("[server] listing topic files");
        (void)send_line(fd, "ERR TOPIC_UNREADABLE\n");
        goto done;
    }

    char header[128];
    (void)snprintf(header, sizeof header, "OK %zu\n", files.count);
    if (send_line(fd, header) != 0) goto release_files;

    for (size_t i = 0; i < files.count; ++i) {
        if (send_file(fd, topic, &files.entries[i], &bytes_sent) != 0) {
            if (errno != EPIPE && errno != ECONNRESET)
                perror("[server] sending file");
            goto release_files;
        }
        files_sent++;
    }
    if (send_line(fd, "DONE\n") != 0) goto release_files;
    success = 1;

release_files:
    topic_list_free(&files);
done:
    {
        double duration = monotonic_seconds() - start;
        double throughput = duration > 0 ? (double)bytes_sent / duration / 1000000.0 : 0;
        fprintf(stderr,
                "[server] status=%s files=%zu bytes=%" PRIu64
                " seconds=%.6f MBps=%.3f\n",
                success ? "OK" : "ERROR", files_sent, bytes_sent,
                duration, throughput);
    }
}

static void *client_thread(void *raw_args) {
    ClientArgs *args = raw_args;
    int fd = args->client_fd;
    free(args);
    handle_client(fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> [single|multi]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port;
    if (validate_port(argv[1], &port) != 0) {
        fprintf(stderr, "[server] Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    int threaded = 1; /* default mode required by the fixed-CLI autograder */
    if (argc == 4) {
        if (strcmp(argv[3], "single") == 0) threaded = 0;
        else if (strcmp(argv[3], "multi") != 0) {
            fprintf(stderr, "[server] Mode must be single or multi\n");
            return EXIT_FAILURE;
        }
    }
    if (topic_map_load(&topic_map, argv[2]) != 0) {
        perror("[server] loading topics");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[server] loaded %zu topics\n", topic_map.count);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop_server;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("[server] installing signal handler");
        return EXIT_FAILURE;
    }
    struct sigaction ignore_sigpipe;
    memset(&ignore_sigpipe, 0, sizeof ignore_sigpipe);
    ignore_sigpipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sigpipe.sa_mask);
    (void)sigaction(SIGPIPE, &ignore_sigpipe, NULL);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) != 0) {
        perror("setsockopt"); close(listen_fd); return EXIT_FAILURE;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_fd, 64) != 0) {
        perror("bind/listen"); close(listen_fd); return EXIT_FAILURE;
    }
    fprintf(stderr, "[server] listening on port %u mode=%s\n",
            (unsigned)port, threaded ? "multi" : "single");

    while (!stopping) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || (stopping && (errno == EBADF || errno == EINVAL))) continue;
            perror("accept");
            break;
        }
        if (!threaded) {
            handle_client(fd);
            close(fd);
            continue;
        }
        ClientArgs *args = malloc(sizeof *args);
        if (args == NULL) { perror("malloc"); close(fd); continue; }
        args->client_fd = fd;
        pthread_t thread;
        int thread_error = pthread_create(&thread, NULL, client_thread, args);
        if (thread_error != 0) {
            errno = thread_error;
            perror("pthread_create");
            close(fd);
            free(args);
            continue;
        }
        (void)pthread_detach(thread);
    }
    if (!stopping && listen_fd >= 0) close(listen_fd);
    fprintf(stderr, "[server] stopped\n");
    return EXIT_SUCCESS;
}
