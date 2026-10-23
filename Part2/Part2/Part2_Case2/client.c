#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "utils.h"
#include "checkpoint.h"
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
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list)) return -1;
    int fd = -1;
    for (struct addrinfo *p = list; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (!connect(fd, p->ai_addr, p->ai_addrlen)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}
static int file_header(char *line, char **name, uint64_t *size) {
    if (strncmp(line, "FILE ", 5)) return -1;
    char *p = strrchr(line + 5, ' ');
    if (!p) return -1;
    *p++ = '\0';
    if (!safe_basename(line + 5) || !*p || *p == '-') return -1;
    char *end; errno = 0;
    unsigned long long v = strtoull(p, &end, 10);
    if (errno || *end) return -1;
    *name = line + 5; *size = (uint64_t)v;
    return 0;
}
/* 0 complete, 1 disconnection, -1 hard error, -2 stale checkpoint. */
static int attempt(int fd, const char *topic, const char *dir, Checkpoint *cp,
                   int resume, uint64_t *received, uint64_t *files_done, uint64_t *useful_size) {
    char line[IT305_MAX_LINE];
    int len = resume ? snprintf(line, sizeof line, "RESUME %s %s %" PRIu64 " %016" PRIx64 "\n",
                  topic, cp->filename, cp->offset, cp->version)
                     : snprintf(line, sizeof line, "GET %s\n", topic);
    if (len <= 0 || (size_t)len >= sizeof line) return -1;
    if (send_all(fd, line, (size_t)len, NULL)) return 1;
    SocketReader reader; reader_init(&reader, fd);
    if (reader_read_line(&reader, line, sizeof line) != 1) return 1;
    if (!strncmp(line, "ERR STALE_CHECKPOINT", 20)) return -2;
    if (!strncmp(line, "ERR ", 4)) { fprintf(stderr, "[client] %s\n", line); return -1; }
    if (!resume) {
        unsigned long long count, version; char extra;
        if (sscanf(line, "OK %llu %llx %c", &count, &version, &extra) != 2) return -1;
        cp->version = (uint64_t)version;
        (void)count;
        cp->offset = 0; cp->filename[0] = '\0'; cp->files_completed = 0;
        printf("[client] topic=%s manifest=%016" PRIx64 "\n", topic, cp->version);
    } else {
        if (strncmp(line, "READY ", 6)) return -1;
        char *value = strrchr(line + 6, ' ');
        if (!value) return -1;
        *value++ = 0;
        if (strcmp(line + 6, cp->filename) || !*value || *value == '-') return -1;
        char *end; errno = 0;
        unsigned long long reply_off = strtoull(value, &end, 10);
        if (errno || *end || (uint64_t)reply_off != cp->offset) return -1;
        printf("[client] RESUME accepted file=%s offset=%" PRIu64 "\n", cp->filename, cp->offset);
    }
    int first = resume;
    for (;;) {
        int r = reader_read_line(&reader, line, sizeof line);
        if (r != 1) return 1;
        if (!strcmp(line, "DONE")) { cp_remove(dir); return 0; }
        char *name; uint64_t sz;
        if (file_header(line, &name, &sz) || strlen(name) >= sizeof cp->filename) return -1;
        char path[PATH_MAX];
        if (join_path(path, sizeof path, dir, name)) return -1;
        uint64_t offset = first ? cp->offset : 0;
        if (offset > sz) return -1;
        first = 0;
        int out = open(path, O_WRONLY|O_CREAT|O_NOFOLLOW, 0644);
        if (out < 0) return -1;
        struct stat st;
        if (fstat(out, &st) || !S_ISREG(st.st_mode) ||
            (uint64_t)st.st_size < offset || ftruncate(out, (off_t)offset) ||
            lseek(out, (off_t)offset, SEEK_SET) < 0) { close(out); return -1; }
        FILE *fp = fdopen(out, "wb");
        if (!fp) { close(out); return -1; }
        if (snprintf(cp->filename, sizeof cp->filename, "%s", name) >= (int)sizeof cp->filename) {
            fclose(fp); return -1;
        }
        cp->offset = offset;
        if (cp_save(dir, cp)) { fclose(fp); return -1; }
        while (cp->offset < sz) {
            uint64_t want = sz - cp->offset;
            if (want > IT305_CHUNK_SIZE) want = IT305_CHUNK_SIZE;
            uint64_t got = 0;
            int status = reader_read_file(&reader, fp, want, &got);
            *received += got;
            if (status) {
                /* Partially received bytes are not committed to checkpoint;
                 * they will be overwritten after reconnection. */
                fclose(fp); return 1;
            }
            if (fflush(fp) || fsync(fileno(fp))) { fclose(fp); return -1; }
            cp->offset += got;
            if (cp_save(dir, cp)) { fclose(fp); return -1; }
        }
        if (fclose(fp)) return -1;
        ++cp->files_completed;
        ++*files_done;
        *useful_size += sz;
        cp->offset = 0;
        /* At file boundary, checkpoint the completed filename with size offset.
         * Retaining the completed file as resume anchor is unambiguous. */
        cp->offset = sz;
        if (cp_save(dir, cp)) return -1;
    }
}
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir> [max_retries]\n", argv[0]);
        return 1;
    }
    uint16_t port;
    if (validate_port(argv[2], &port) || !safe_basename(argv[3]) ||
        strchr(argv[3], '.') || strlen(argv[3]) >= 64) return 1;
    int maximum = 10;
    if (argc == 6) {
        char *end; errno = 0; long value = strtol(argv[5], &end, 10);
        if (errno || *end || end == argv[5] || value < 0 || value > 1000) return 1;
        maximum = (int)value;
    }
    char dir[PATH_MAX];
    if (join_path(dir, sizeof dir, argv[4], argv[3]) || mkdir_p(dir)) return 1;
    Checkpoint cp = {0};
    (void)snprintf(cp.topic, sizeof cp.topic, "%s", argv[3]);
    int resume = cp_load(dir, &cp) == 0 && !strcmp(cp.topic, argv[3]);
    uint64_t bytes = 0, completed = 0, useful = 0;
    int retries = 0, success = 0;
    double start = monotonic_seconds();
    for (int i = 0; i <= maximum; ++i) {
        int fd = connect_to_server(argv[1], argv[2]);
        int rc = fd < 0 ? 1 : attempt(fd, argv[3], dir, &cp, resume, &bytes, &completed, &useful);
        if (fd >= 0) close(fd);
        if (!rc) { success = 1; break; }
        if (rc == -2) {
            fprintf(stderr, "[client] STALE CHECKPOINT: starting fresh\n");
            cp_remove(dir); resume = 0; cp.offset = 0; cp.filename[0] = 0;
        } else if (rc < 0) break;
        else resume = cp.filename[0] != '\0';
        if (i == maximum) break;
        ++retries;
        fprintf(stderr, "[client] disconnected, reconnecting (retry %d/%d)\n", retries, maximum);
        unsigned delay = (unsigned)(1u << (retries <= 3 ? retries - 1 : 2));
        sleep(delay);
    }
    double seconds = monotonic_seconds() - start;
    printf("METRIC mode=case2 status=%s files=%" PRIu64 " received_bytes=%" PRIu64
           " useful_bytes=%" PRIu64 " retries=%d seconds=%.6f MBps=%.3f\n",
           success ? "OK" : "ERROR", completed, bytes, useful, retries, seconds,
           seconds > 0 ? (double)useful / seconds / 1000000.0 : 0.0);
    return success ? 0 : 1;
}
