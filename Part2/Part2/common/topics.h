#ifndef IT305_TOPICS_H
#define IT305_TOPICS_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef struct {
    char key[IT305_MAX_TOPIC];
    char dir_path[PATH_MAX];
} TopicEntry;

typedef struct {
    TopicEntry entries[IT305_MAX_TOPICS];
    size_t count;
} TopicMap;

typedef struct {
    char *name;
    uint64_t size;
} TopicFile;

typedef struct {
    TopicFile *entries;
    size_t count;
} TopicFileList;

int topic_map_load(TopicMap *map, const char *root_dir);
const TopicEntry *topic_map_find(const TopicMap *map, const char *key);
int topic_list_files(const TopicEntry *topic, TopicFileList *files);
void topic_list_free(TopicFileList *files);

#endif
