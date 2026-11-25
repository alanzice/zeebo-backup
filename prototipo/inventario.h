#ifndef ZEEBO_INVENTARIO_H
#define ZEEBO_INVENTARIO_H

#include <stddef.h>

enum { PATH_CAPACITY = 128, MAX_DEPTH = 4, MAX_DIRECTORIES = 24, MAX_ENTRIES = 128 };

typedef enum {
    PROBE_OK, PROBE_END, PROBE_MISSING, PROBE_DENIED, PROBE_NO_MEDIA,
    PROBE_ERROR, PROBE_LIMIT, PROBE_SKIPPED
} ProbeStatus;

typedef struct {
    ProbeStatus status;
    int code;
} ProbeResult;

typedef struct {
    char name[PATH_CAPACITY];
    unsigned long size;
    int directory;
} Entry;

typedef struct {
    void *context;
    ProbeResult (*begin_list)(void *context, const char *path);
    ProbeResult (*next_entry)(void *context, Entry *entry);
    void (*end_list)(void *context);
    ProbeResult (*read_prefix)(void *context, const char *path, size_t *bytes);
} ReadOnlyFs;

typedef enum { LIST_RESULT, ENTRY_RESULT, READ_RESULT } EventKind;

typedef struct {
    EventKind kind;
    const char *path;
    ProbeResult result;
    unsigned long bytes;
    int directory;
} Event;

typedef void (*Report)(void *context, const Event *event);

typedef struct {
    char path[PATH_CAPACITY];
    unsigned depth;
} Directory;

typedef struct {
    ReadOnlyFs fs;
    Report report;
    void *report_context;
    Directory pending[MAX_DIRECTORIES];
    unsigned next_directory;
    unsigned directory_count;
    unsigned entries;
    int listing;
    int finished;
    char read_path[PATH_CAPACITY];
} Inventory;

int inventory_start(Inventory *inventory, ReadOnlyFs fs, Report report, void *context,
                    const char *game_id, const char *sample_name);
int inventory_step(Inventory *inventory);
void inventory_cancel(Inventory *inventory);

#endif
