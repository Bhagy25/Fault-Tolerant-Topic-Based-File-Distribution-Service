#define _POSIX_C_SOURCE 200809L
#include "topics.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int topic_map_load(TopicMap *map, const char *root_dir) {
    char resolved[PATH_MAX];
    if (realpath(root_dir, resolved) == NULL) return -1;
    DIR *dir = opendir(resolved);
    if (dir == NULL) return -1;
    map->count = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!safe_basename(entry->d_name) || strchr(entry->d_name, '.') != NULL) continue;
        char absolute[PATH_MAX];
        if (join_path(absolute, sizeof absolute, resolved, entry->d_name) != 0) goto fail;
        struct stat st;
        if (lstat(absolute, &st) != 0) goto fail;
        if (!S_ISDIR(st.st_mode)) continue; /* Ignore files and symlinked directories. */
        if (strlen(entry->d_name) >= IT305_MAX_TOPIC || map->count >= IT305_MAX_TOPICS) {
            errno = EOVERFLOW;
            goto fail;
        }
        TopicEntry *target = &map->entries[map->count++];
        snprintf(target->key, sizeof target->key, "%s", entry->d_name);
        snprintf(target->dir_path, sizeof target->dir_path, "%s", absolute);
        errno = 0;
    }
    if (errno != 0) goto fail;
    if (closedir(dir) != 0) return -1;
    return 0;
fail:
    {
        int saved_errno = errno;
        closedir(dir);
        errno = saved_errno;
        return -1;
    }
}

const TopicEntry *topic_map_find(const TopicMap *map, const char *key) {
    if (!safe_basename(key) || strchr(key, '.') != NULL) return NULL;
    for (size_t i = 0; i < map->count; ++i)
        if (strcmp(map->entries[i].key, key) == 0) return &map->entries[i];
    return NULL;
}

static int compare_files(const void *a, const void *b) {
    return strcmp(((const TopicFile *)a)->name, ((const TopicFile *)b)->name);
}

void topic_list_free(TopicFileList *files) {
    for (size_t i = 0; i < files->count; ++i) free(files->entries[i].name);
    free(files->entries);
    files->entries = NULL;
    files->count = 0;
}

int topic_list_files(const TopicEntry *topic, TopicFileList *files) {
    files->entries = NULL;
    files->count = 0;
    size_t allocated = 0;
    DIR *dir = opendir(topic->dir_path);
    if (dir == NULL) return -1;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!safe_basename(entry->d_name)) continue;
        char filepath[PATH_MAX];
        if (join_path(filepath, sizeof filepath, topic->dir_path, entry->d_name) != 0) goto fail;
        struct stat st;
        if (lstat(filepath, &st) != 0) goto fail;
        if (!S_ISREG(st.st_mode)) { errno = 0; continue; }
        if (st.st_size < 0) { errno = EINVAL; goto fail; }
        if (files->count == allocated) {
            size_t new_size = allocated ? allocated * 2 : 32;
            if (new_size < allocated || new_size > SIZE_MAX / sizeof(TopicFile)) {
                errno = ENOMEM; goto fail;
            }
            TopicFile *new_entries = realloc(files->entries, new_size * sizeof(TopicFile));
            if (new_entries == NULL) goto fail;
            files->entries = new_entries;
            allocated = new_size;
        }
        char *name = strdup(entry->d_name);
        if (name == NULL) goto fail;
        files->entries[files->count].name = name;
        files->entries[files->count].size = (uint64_t)st.st_size;
        files->count++;
        errno = 0;
    }
    if (errno != 0) goto fail;
    if (closedir(dir) != 0) { topic_list_free(files); return -1; }
    qsort(files->entries, files->count, sizeof(TopicFile), compare_files);
    return 0;
fail:
    {
        int saved_errno = errno;
        closedir(dir);
        topic_list_free(files);
        errno = saved_errno;
        return -1;
    }
}
