#ifndef ZEEBO_BACKUP_SCREEN_H
#define ZEEBO_BACKUP_SCREEN_H

#include "backup.h"

typedef enum { HOME, GAMES, CONFIRM, RUNNING, RESULTS, SD_DETAILS, INSTRUCTIONS, RESUME_BACKUP, FILES_UNAVAILABLE } Screen;
enum { HOME_FULL, HOME_GAME, HOME_HELP, HOME_RESUME, GAME_ROWS = 6 };

typedef struct {
    Screen screen;
    unsigned home_choice;
    unsigned home_taps;
    unsigned game_choice;
    unsigned first_game;
    int selected_game;
    int confirm_copy;
    uint32_t sd_available;
    int sd_space_known;
    int startup_error;
    int timer_error;
    int dark_theme;
    int paused, resume_error;
    unsigned activity_frame;
    uint32_t copy_elapsed_ms;
    int clock_available;
    uint32_t resume_size;
    char resume_path[PATH_SIZE];
} View;

void render_backup(BrewObject *display, const View *view, const Catalog *catalog, const Backup *backup);

#endif
