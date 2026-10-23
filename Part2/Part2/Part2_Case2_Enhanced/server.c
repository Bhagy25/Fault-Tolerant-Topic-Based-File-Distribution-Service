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
static double fault_probability = 0.0;
static pthread_mutex_t fault_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t fault_state = UINT64_C(0x9e3779b97f4a7c15);

static int inject_fault(void) {
    if (fault_probability <= 0.0) return 0;
    if (fault_probability >= 1.0) return 1;
    pthread_mutex_lock(&fault_mutex);
    fault_state ^= fault_state >> 12;
    fault_state ^= fault_state << 25;
    fault_state ^= fault_state >> 27;
    uint64_t value = fault_state * UINT64_C(2685821657736338717);
    pthread_mutex_unlock(&fault_mutex);
    return (double)(value >> 11) / 9007199254740992.0 < fault_probability;
}


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
                     uint64_t offset, uint64_t *file_bytes_sent) {
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

    if (offset > file->size || lseek(source, (off_t)offset, SEEK_SET) < 0) { close(source); return -1; }
    char header[IT305_MAX_LINE];
    int len = snprintf(header, sizeof header, "FILE %s %" PRIu64 "\n", file->name, file->size);
    if (len < 0 || (size_t)len >= sizeof header || send_line(fd, header) != 0) {
        close(source);
        return -1;
    }

    unsigned char buffer[IT305_CHUNK_SIZE];
    uint64_t remaining = file->size - offset;
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
        if (inject_fault()) {
            fprintf(stderr, "[server] FAULT injected after chunk file=%s bytes=%" PRIu64 "\n", file->name, *file_bytes_sent);
            errno = ECONNRESET;
            close(source);
            return -2;
        }
    }
    return close(source);
}

/* FNV-1a over sorted names and file sizes: a stable manifest version.
 * A source file's same-size content alteration is NOT detected by this
 * lightweight manifest; production deployment would use content hashes. */
static uint64_t manifest_hash(const TopicFileList *list) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < list->count; ++i) {
        const unsigned char *s = (const unsigned char *)list->entries[i].name;
        while (*s) { h ^= *s++; h *= UINT64_C(1099511628211); }
        h ^= 0xff; h *= UINT64_C(1099511628211);
        uint64_t sz = list->entries[i].size;
        for (int k = 0; k < 8; ++k) { h ^= (unsigned char)(sz >> (k*8)); h *= UINT64_C(1099511628211); }
    }
    return h;
}

static int parse_resume(char *request, char **topic_name, char **file_name,
                        uint64_t *offset, uint64_t *version) {
    /* Whitespace is allowed within filenames; parse last two fields backwards. */
    if (strncmp(request, "RESUME ", 7) != 0) return -1;
    char *a = request + 7, *space = strchr(a, ' ');
    if (!space) return -1;
    *space++ = 0;
    if (!safe_basename(a) || strchr(a, '.')) return -1;
    char *last = strrchr(space, ' ');
    if (!last) return -1;
    *last++ = 0;
    char *penultimate = strrchr(space, ' ');
    if (!penultimate) return -1;
    *penultimate++ = 0;
    if (!safe_basename(space)) return -1;
    char *end; errno = 0;
    unsigned long long o = strtoull(penultimate, &end, 10);
    if (errno || *end || end == penultimate || *penultimate == '-') return -1;
    errno = 0;
    unsigned long long v = strtoull(last, &end, 16);
    if (errno || *end || end == last || *last == '-') return -1;
    *topic_name = a; *file_name = space; *offset = (uint64_t)o; *version = (uint64_t)v;
    return 0;
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

    int parallel = strncmp(request, "PARALLEL ", 9) == 0;
    int parallel_resume = strncmp(request, "RESUME PARALLEL ", 16) == 0;
    int metadata = strncmp(request, "META ", 5) == 0;
    int resume = !parallel_resume && strncmp(request, "RESUME ", 7) == 0;
    unsigned nconn = 0, connid = 0;
    char parallel_topic[IT305_MAX_TOPIC] = {0};
    char parallel_filename[256] = {0};
    uint64_t parallel_offset = 0, parallel_version = 0;
    /* Parse PARALLEL / RESUME PARALLEL strictly, with trailing filename and offset. */
    if (parallel || parallel_resume) {
        unsigned long long off = 0, ver = 0;
        if (parallel) {
            char extra;
            if (sscanf(request,"PARALLEL %63s %u %u %c",parallel_topic,&nconn,&connid,&extra)!=3) {
                (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done;
            }
        } else {
            /* In this protocol source filenames with spaces are supported by
             * consuming the last offset/version tokens from the right. */
            char *cursor = request + 16;
            char *one = strchr(cursor,' ');
            if(!one || (size_t)(one-cursor)>=sizeof parallel_topic) { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            memcpy(parallel_topic,cursor,(size_t)(one-cursor));
            char *next = one+1, *end;
            errno=0; unsigned long count = strtoul(next,&end,10);
            if(errno || end==next || *end!=' ') { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            nconn=(unsigned)count; next=end+1;
            errno=0; unsigned long id=strtoul(next,&end,10);
            if(errno || end==next || *end!=' ') { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            connid=(unsigned)id; next=end+1;
            char *last = strrchr(next,' ');
            if(!last) { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            *last++=0;
            char *before = strrchr(next,' ');
            if(!before) { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            *before++=0;
            if(strlen(next)>=sizeof parallel_filename) { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            strcpy(parallel_filename,next);
            char *finish; errno=0;
            off=strtoull(before,&finish,10);
            if(errno || finish==before || *finish || *before=='-') { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            errno=0; ver=strtoull(last,&finish,16);
            if(errno || finish==last || *finish || *last=='-') { (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done; }
            parallel_offset=off; parallel_version=ver;
        }
        if (nconn<1 || nconn>32 || connid>=nconn || !safe_basename(parallel_topic) || strchr(parallel_topic,'.') ||
            (parallel_resume && !safe_basename(parallel_filename))) {
            (void)send_line(fd,"ERR BAD_PARALLEL\n"); goto done;
        }
    }
    char *requested_topic = NULL, *resume_filename = NULL;
    uint64_t resume_offset = 0, resume_version = 0;
    if (parallel || parallel_resume) { requested_topic = parallel_topic; }
    else if (metadata) { requested_topic = request + 5; }
    else if (resume) {
        if (parse_resume(request, &requested_topic, &resume_filename,
                         &resume_offset, &resume_version) != 0) {
            (void)send_line(fd, "ERR INVALID_RESUME\n"); goto done;
        }
    } else if (strncmp(request, "GET ", 4) == 0 && request[4] != 0) {
        requested_topic = request + 4;
    } else {
        (void)send_line(fd, "ERR INVALID_REQUEST\n"); goto done;
    }
    const TopicEntry *topic = topic_map_find(&topic_map, requested_topic);
    if (!topic) { (void)send_line(fd, "ERR UNKNOWN_TOPIC\n"); goto done; }
    TopicFileList files;
    if (topic_list_files(topic, &files) != 0) {
        (void)send_line(fd, "ERR TOPIC_UNREADABLE\n"); goto done;
    }
    uint64_t version = manifest_hash(&files);
    if (metadata) {
        char meta[128];
        snprintf(meta,sizeof meta,"META %zu %016" PRIx64 "\n", files.count,version);
        (void)send_line(fd,meta);
        goto release_files;
    }
    size_t first_index = 0;
    size_t end_index = files.count;
    if (parallel || parallel_resume) {
        /* Balanced contiguous whole-file ranges, including empty ranges. */
        first_index = (files.count * connid) / nconn;
        end_index = (files.count * (connid + 1)) / nconn;
        if (parallel_resume) {
            if (version != parallel_version) { (void)send_line(fd,"ERR STALE_CHECKPOINT\n"); goto release_files; }
            size_t i=first_index;
            while(i<end_index && strcmp(files.entries[i].name,parallel_filename)) ++i;
            if(i == end_index || parallel_offset > files.entries[i].size) {
                (void)send_line(fd,"ERR STALE_CHECKPOINT\n"); goto release_files;
            }
            first_index=i;
        }
    }
    if (resume) {
        if (version != resume_version) {
            (void)send_line(fd, "ERR STALE_CHECKPOINT\n"); goto release_files;
        }
        while (first_index < files.count &&
               strcmp(files.entries[first_index].name, resume_filename) != 0) ++first_index;
        if (first_index == files.count || resume_offset > files.entries[first_index].size) {
            (void)send_line(fd, "ERR STALE_CHECKPOINT\n"); goto release_files;
        }
    }

    char header[256];
    int n;
    if(parallel_resume) n=snprintf(header,sizeof header,"READY %s %" PRIu64 "\n",parallel_filename,parallel_offset);
    else if(parallel) n=snprintf(header,sizeof header,"ASSIGN %zu %zu %016" PRIx64 "\n",
                                 (files.count*connid)/nconn,end_index,version);
    else if(resume) n=snprintf(header,sizeof header,"READY %s %" PRIu64 "\n",resume_filename,resume_offset);
    else n=snprintf(header,sizeof header,"OK %zu %016" PRIx64 "\n",files.count,version);
    if (n < 0 || (size_t)n >= sizeof header || send_line(fd, header) != 0) goto release_files;
    for (size_t i = first_index; i < end_index; ++i) {
        uint64_t offset = parallel_resume && i == first_index ? parallel_offset : (resume && i == first_index ? resume_offset : 0);
        int result = send_file(fd, topic, &files.entries[i], offset, &bytes_sent);
        if (result != 0) {
            if (result != -2 && errno != EPIPE && errno != ECONNRESET)
                perror("[server] sending file");
            goto release_files;
        }
        files_sent++;
    }
    if (send_line(fd, parallel || parallel_resume ? "CONN DONE\n" : "DONE\n") != 0) goto release_files;
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
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability>\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port;
    if (validate_port(argv[1], &port) != 0) {
        fprintf(stderr, "[server] Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    char *end_probability = NULL;
    errno = 0;
    fault_probability = strtod(argv[3], &end_probability);
    if (errno != 0 || argv[3][0] == '\0' || end_probability == argv[3] ||
        *end_probability != '\0' || !(fault_probability >= 0.0 && fault_probability <= 1.0)) {
        fprintf(stderr, "[server] failure_probability must be between 0 and 1\n");
        return EXIT_FAILURE;
    }
    fault_state ^= (uint64_t)(monotonic_seconds() * 1e9) ^ (uint64_t)getpid();
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
    fprintf(stderr, "[server] listening on port %u mode=multi failure_probability=%.6f\n",
            (unsigned)port, fault_probability);

    while (!stopping) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || (stopping && (errno == EBADF || errno == EINVAL))) continue;
            perror("accept");
            break;
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
