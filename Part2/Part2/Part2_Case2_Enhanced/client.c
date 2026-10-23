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
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

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
    if(fd >= 0) {
        struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
        if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)!=0) {
            close(fd); return -1;
        }
    }
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
                   int resume, uint64_t *received, uint64_t *files_done, uint64_t *useful_size,
                   const char *checkpoint_dir, unsigned nconn, unsigned connid) {
    char line[IT305_MAX_LINE];
    int len = resume ? snprintf(line, sizeof line, "RESUME PARALLEL %s %u %u %s %" PRIu64 " %016" PRIx64 "\n",
                  topic,nconn,connid,cp->filename,cp->offset,cp->version)
                     : snprintf(line, sizeof line, "PARALLEL %s %u %u\n",topic,nconn,connid);
    if (len <= 0 || (size_t)len >= sizeof line) return -1;
    if (send_all(fd, line, (size_t)len, NULL)) return 1;
    SocketReader reader; reader_init(&reader, fd);
    if (reader_read_line(&reader, line, sizeof line) != 1) return 1;
    if (!strncmp(line, "ERR STALE_CHECKPOINT", 20)) return -2;
    if (!strncmp(line, "ERR ", 4)) { fprintf(stderr, "[client] %s\n", line); return -1; }
    if (!resume) {
        unsigned long long from, to, version; char extra;
        if (sscanf(line, "ASSIGN %llu %llu %llx %c", &from, &to, &version, &extra) != 3) return -1;
        if (to < from) return -1;
        cp->version = (uint64_t)version;
        (void)from; (void)to;
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
        if (!strcmp(line, "CONN DONE")) { cp_remove(checkpoint_dir); return 0; }
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
        if (cp_save(checkpoint_dir, cp)) { fclose(fp); return -1; }
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
            if (cp_save(checkpoint_dir, cp)) { fclose(fp); return -1; }
        }
        if (fclose(fp)) return -1;
        ++cp->files_completed;
        ++*files_done;
        *useful_size += sz;
        cp->offset = 0;
        /* At file boundary, checkpoint the completed filename with size offset.
         * Retaining the completed file as resume anchor is unambiguous. */
        cp->offset = sz;
        if (cp_save(checkpoint_dir, cp)) return -1;
    }
}
typedef struct {
    const char *host, *port, *topic;
    char output_dir[PATH_MAX], checkpoint_dir[PATH_MAX];
    unsigned id, count;
    int max_retries, success;
    uint64_t bytes, completed, useful;
    int retries;
} Worker;
static void *worker_main(void *argument) {
    Worker *w=argument;
    Checkpoint cp={0};
    snprintf(cp.topic,sizeof cp.topic,"%s",w->topic);
    int resume=cp_load(w->checkpoint_dir,&cp)==0 && !strcmp(cp.topic,w->topic);
    for(int i=0;i<=w->max_retries;++i) {
        int fd=connect_to_server(w->host,w->port);
        int rc=fd<0 ? 1 : attempt(fd,w->topic,w->output_dir,&cp,resume,&w->bytes,&w->completed,&w->useful,w->checkpoint_dir,w->count,w->id);
        if(fd>=0) close(fd);
        if(rc==0) { w->success=1; break; }
        if(rc==-2) {
            fprintf(stderr,"[parallel %u] stale checkpoint: restarting assigned range\n",w->id);
            cp_remove(w->checkpoint_dir); resume=0; cp.filename[0]=0; cp.offset=0;
            /* Invalidate old progress and immediately restart the range;
             * stale metadata isn't treated as a transient network retry. */
            fd=connect_to_server(w->host,w->port);
            rc=fd<0 ? 1 : attempt(fd,w->topic,w->output_dir,&cp,0,&w->bytes,&w->completed,
                                     &w->useful,w->checkpoint_dir,w->count,w->id);
            if(fd>=0) close(fd);
            if(rc==0) { w->success=1; break; }
            if(rc<0) break;
            resume=cp.filename[0]!=0;
        }
        else if(rc<0) break;
        else resume=cp.filename[0]!=0;
        if(i==w->max_retries) break;
        ++w->retries;
        fprintf(stderr,"[parallel %u] reconnect retry %d\n",w->id,w->retries);
        sleep((unsigned)(1u << (w->retries<=3?w->retries-1:2)));
    }
    return NULL;
}
int main(int argc,char **argv) {
    if(argc!=5 && argc!=6 && argc!=7) {
        fprintf(stderr,"Usage: %s <server_ip> <server_port> <topic> <output_dir> [connections] [max_retries]\n",argv[0]); return 1;
    }
    uint16_t port;
    if(validate_port(argv[2],&port) || !safe_basename(argv[3]) || strchr(argv[3],'.') || strlen(argv[3])>=64) return 1;
    unsigned n=4; int max_retries=10;
    if(argc>=6) { char *end; errno=0; unsigned long value=strtoul(argv[5],&end,10); if(errno || !*argv[5] || *end || value<1 || value>32) return 1; n=(unsigned)value; }
    if(argc==7) { char *end; errno=0; long value=strtol(argv[6],&end,10); if(errno || !*argv[6] || *end || value<0 || value>1000) return 1; max_retries=(int)value; }
    char output_dir[PATH_MAX];
    if(join_path(output_dir,sizeof output_dir,argv[4],argv[3]) || mkdir_p(output_dir)) return 1;
    /* Dedicated metadata/control connection. */
    int control=connect_to_server(argv[1],argv[2]);
    if(control<0) { perror("connect control"); return 1; }
    char line[IT305_MAX_LINE];
    snprintf(line,sizeof line,"META %s\n",argv[3]);
    if(send_all(control,line,strlen(line),NULL)) { close(control); return 1; }
    SocketReader rd; reader_init(&rd,control);
    if(reader_read_line(&rd,line,sizeof line)!=1) {close(control);return 1;}
    close(control);
    unsigned long long count,version;
    if(sscanf(line,"META %llu %llx",&count,&version)!=2) {
        fprintf(stderr,"[client] metadata failure: %s\n",line); return 1;
    }
    printf("[client] topic=%s files=%llu manifest=%016llx connections=%u\n",argv[3],count,version,n);
    Worker *workers=calloc(n,sizeof *workers);
    pthread_t *threads=calloc(n,sizeof *threads);
    if(!workers || !threads) {free(workers);free(threads);return 1;}
    double start=monotonic_seconds();
    unsigned started=0;
    for(unsigned i=0;i<n;++i) {
        Worker *w=&workers[i];
        w->host=argv[1];w->port=argv[2];w->topic=argv[3];w->id=i;w->count=n;w->max_retries=max_retries;
        snprintf(w->output_dir,sizeof w->output_dir,"%s",output_dir);
        char subdir[32]; snprintf(subdir,sizeof subdir,".parallel_%u",i);
        if(join_path(w->checkpoint_dir,sizeof w->checkpoint_dir,output_dir,subdir) || mkdir_p(w->checkpoint_dir)) break;
        if(pthread_create(&threads[i],NULL,worker_main,w)!=0) break;
        ++started;
    }
    for(unsigned i=0;i<started;++i) pthread_join(threads[i],NULL);
    int success=started==n; uint64_t received=0, useful=0, completed=0; int retries=0;
    for(unsigned i=0;i<n;++i) {
        success=success && workers[i].success;
        received+=workers[i].bytes; useful+=workers[i].useful; completed+=workers[i].completed; retries+=workers[i].retries;
    }
    double seconds=monotonic_seconds()-start;
    printf("METRIC mode=enhanced status=%s files=%" PRIu64 " received_bytes=%" PRIu64
           " useful_bytes=%" PRIu64 " retries=%d seconds=%.6f MBps=%.3f\n",
           success?"OK":"ERROR",completed,received,useful,retries,seconds,
           seconds>0?(double)useful/seconds/1000000.0:0.0);
    free(threads);free(workers);
    return success?0:1;
}
