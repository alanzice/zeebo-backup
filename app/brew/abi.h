#ifndef ZEEBO_BREW_ABI_H
#define ZEEBO_BREW_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Prefixos usados pelo applet. Referências e limites estão no README. */
typedef struct BrewObject BrewObject;
typedef void (*BrewSlot)(void);
typedef void (*BrewTimer)(void *);

struct BrewObject {
    const BrewSlot *vt;
};

typedef struct {
    uint8_t attributes;
    uint8_t padding[3];
    uint32_t created;
    uint32_t size;
    char name[64];
} BrewFileInfo;

typedef char check_file_info_size[sizeof(BrewFileInfo) == 76 ? 1 : -1];

enum {
    FILEMGR_CLASS = 0x01001003, DISPLAY_CLASS = 0x01001001,
    ADD_REF_SLOT = 0, RELEASE_SLOT = 1,
    CREATE_INSTANCE_SLOT = 2, CLOSE_APPLET_SLOT = 6, SET_TIMER_SLOT = 11, CANCEL_TIMER_SLOT = 12,
    FILE_OPEN_SLOT = 2, FILE_INFO_SLOT = 3, FILE_REMOVE_SLOT = 4, FILE_MKDIR_SLOT = 5,
    FILE_TEST_SLOT = 7, FILE_ERROR_SLOT = 9, ENUM_INIT_SLOT = 10, ENUM_NEXT_SLOT = 11,
    FILE_RENAME_SLOT = 12, ENUM_NEXT_EX_SLOT = 13, FILE_READ_SLOT = 3, FILE_WRITE_SLOT = 5,
    FILE_SEEK_SLOT = 7, FILE_SEEK_START = 0,
    FILE_FREE_SPACE_EX_SLOT = 20,
    DRAW_TEXT_SLOT = 4, DRAW_RECT_SLOT = 5, UPDATE_SLOT = 7, SET_COLOR_SLOT = 10,
    HELP_ALLOC_SLOT = 26, HELP_FREE_SLOT = 27, HELP_UPTIME_SLOT = 44,
    FILE_READ_ONLY = 1, FILE_CREATE = 4, FILE_APPEND = 8, ATTR_DIRECTORY = 2,
    APP_START = 0, APP_STOP = 1, APP_SUSPEND = 2, APP_RESUME = 3,
    BREW_FAILED = 1, BREW_BAD_PARAM = 2, BREW_NO_MEMORY = 3,
    BREW_PRIVILEGE_ERROR = 21, BREW_FILE_FULL = 262,
    BREW_FILE_NOT_FOUND = 257, BREW_DIR_NOT_FOUND = 265,
    BREW_FILE_INVALID_OPERATION = 266, BREW_FILE_NO_MEDIA = 267,
    RECT_FILL = 2, FONT_NORMAL = 0, COLOR_USER_TEXT = 1, TEXT_TRANSPARENT = 0x8000
};

static inline unsigned brew_ref(BrewObject *object, unsigned slot)
{
    return ((unsigned (*)(BrewObject *))object->vt[slot])(object);
}

static inline int brew_create(BrewObject *shell, unsigned id, BrewObject **out)
{
    return ((int (*)(BrewObject *, unsigned, BrewObject **))shell->vt[CREATE_INSTANCE_SLOT])(
        shell, id, out);
}

#endif
