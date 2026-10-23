#define _POSIX_C_SOURCE 200809L
#include "checkpoint.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>

static int cp_path(char *out, size_t sz, const char *dir, const char *suffix) {
    int n = snprintf(out, sz, "%s/.checkpoint_case2%s", dir, suffix);
    return n > 0 && (size_t)n < sz ? 0 : -1;
}
int cp_save(const char *dir, const Checkpoint *cp) {
    char final[PATH_MAX], temp[PATH_MAX];
    if (cp_path(final, sizeof final, dir, "") || cp_path(temp, sizeof temp, dir, ".tmp")) return -1;
    int fd = open(temp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s\n%s\n%" PRIu64 "\n%016" PRIx64 "\n%" PRIu64 "\n",
                     cp->topic, cp->filename, cp->offset, cp->version, cp->files_completed);
    if (n <= 0 || (size_t)n >= sizeof buf || write(fd, buf, (size_t)n) != n || fsync(fd)) {
        close(fd); unlink(temp); return -1;
    }
    if (close(fd) || rename(temp, final)) { unlink(temp); return -1; }
    return 0;
}
int cp_load(const char *dir, Checkpoint *cp) {
    char path[PATH_MAX];
    if (cp_path(path, sizeof path, dir, "")) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }
    unsigned long long off, ver, count;
    int ok = fscanf(fp, "%63[^\n]\n%255[^\n]\n%llu\n%llx\n%llu", cp->topic,
                    cp->filename, &off, &ver, &count) == 5;
    fclose(fp);
    if (!ok || !safe_basename(cp->filename)) return -1;
    cp->offset = (uint64_t)off; cp->version = (uint64_t)ver;
    cp->files_completed = (uint64_t)count;
    return 0;
}
void cp_remove(const char *dir) {
    char path[PATH_MAX];
    if (!cp_path(path, sizeof path, dir, "")) unlink(path);
}
