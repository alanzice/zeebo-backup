#ifndef ZEEBO_BACKUP_H
#define ZEEBO_BACKUP_H

#include "../brew/abi.h"

enum { GAME_COUNT = 128, GAME_ID_SIZE = 49, TITLE_SIZE = 65, PATH_SIZE = 128,
       DIR_DEPTH = 12, COPY_BLOCK = 16384 };

typedef struct {
    char games[GAME_COUNT][GAME_ID_SIZE];
    char titles[GAME_COUNT][TITLE_SIZE];
    uint8_t title_checked[GAME_COUNT];
    unsigned count;
    unsigned omitted;
    int error;
} Catalog;

typedef struct {
    BrewObject *files;
    char path[PATH_SIZE];
    uint8_t directories;
} Folder;

typedef enum { BACKUP_IDLE, BACKUP_RUNNING, BACKUP_DONE, BACKUP_PARTIAL, BACKUP_CANCELLED } BackupState;
typedef enum { COPY_NONE, COPY_STREAM, COPY_VERIFY, COPY_REUSE_SOURCE, COPY_REUSE_SD } CopyPhase;

enum { RESUME_SLOTS = 16384, BACKUP_DESTINATION_CONFLICT = -4 };

typedef struct {
    BrewObject *shell;
    BrewObject *files;
    BrewObject *source;
    BrewObject *output;
    BrewObject *log;
    BrewObject *checkpoint;
    uint32_t *resume_index;
    char *checkpoint_text;
    uint32_t checkpoint_size, checkpoint_offset;
    uint32_t checkpoint_loaded;
    unsigned remaining_paths, reused, zero_files;
    int reading_checkpoint, replacing, source_nonzero;
    const Catalog *catalog;
    BackupState state;
    CopyPhase phase;
    Folder folders[DIR_DEPTH];
    unsigned depth;
    unsigned game;
    unsigned game_end;
    unsigned task;
    unsigned config;
    unsigned copied;
    unsigned errors;
    unsigned excluded;
    unsigned system_missing;
    int last_error_is_system;
    int include_config;
    int planning;
    uint64_t planned_bytes;
    uint64_t verified_bytes;
    int last_code;
    const char *last_operation;
    int log_error;
    int history_error;
    int history_skipped;
    int sd_setup_failed;
    int sd_info_code;
    int sd_marker_code;
    int sd_last_error;
    int sd_volume_code;
    int temporary_owned;
    char session[PATH_SIZE];
    char temporary[PATH_SIZE];
    char current[PATH_SIZE];
    char destination[PATH_SIZE];
    char last_error[PATH_SIZE];
    uint32_t expected;
    uint32_t transferred;
    uint32_t crc;
    uint32_t verify_crc;
    uint32_t crc_table[256];
    uint8_t buffer[COPY_BLOCK];
} Backup;

void catalog_load(BrewObject *files, Catalog *catalog);
void catalog_load_title(BrewObject *files, Catalog *catalog, unsigned index);
void catalog_label(const Catalog *catalog, unsigned index, char *label, unsigned limit);
int sd_free_space(BrewObject *files, uint32_t *available);
uint32_t backup_find_resume(BrewObject *files, char *path);
void backup_start(Backup *backup, BrewObject *shell, BrewObject *files, const Catalog *catalog,
                  int selected_game, const char *resume_path, uint32_t *resume_index, uint32_t checkpoint_capacity);
void backup_step(Backup *backup);
void backup_cancel(Backup *backup);
unsigned text_add(char *out, unsigned at, const char *value, unsigned limit);
unsigned text_number(char *out, unsigned at, uint32_t number);
const char *game_name(const char *id);
int numeric_id(const char *id);

#endif
