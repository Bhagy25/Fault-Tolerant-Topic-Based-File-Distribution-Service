#ifndef IT305_CHECKPOINT_H
#define IT305_CHECKPOINT_H
#include <stdint.h>
#include <limits.h>
typedef struct {
    char topic[64];
    char filename[256];
    uint64_t offset;
    uint64_t version;
    uint64_t files_completed;
} Checkpoint;
int cp_save(const char *dir, const Checkpoint *cp);
int cp_load(const char *dir, Checkpoint *cp);
void cp_remove(const char *dir);
#endif
