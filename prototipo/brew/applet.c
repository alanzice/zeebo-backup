#include "abi.h"
#include "../inventario.h"

#ifndef INVENTORY_CLASS
#error Defina INVENTORY_CLASS ao compilar.
#endif
#ifndef GAME_ID
#error Defina GAME_ID ao compilar.
#endif
#define STRING_VALUE(token) #token
#define STRING_FROM(token) STRING_VALUE(token)

enum { MAX_ROWS = MAX_DIRECTORIES + 1, READ_BYTES = 256, STEPS_PER_TICK = 8 };

typedef void *(*Allocate)(size_t);
typedef void (*Deallocate)(void *);

typedef struct {
    BrewObject object;
    BrewSlot table[4];
    BrewObject *shell;
    Allocate allocate;
    Deallocate free;
    unsigned refs;
} Module;

typedef struct {
    char path[PATH_CAPACITY];
    ProbeResult result;
    unsigned long count;
    EventKind kind;
} Row;

typedef struct {
    BrewObject object;
    BrewSlot table[3];
    Module *module;
    BrewObject *shell;
    BrewObject *display;
    BrewObject *files;
    unsigned refs;
    unsigned row_count;
    unsigned omitted_rows;
    unsigned entry_count;
    int scan_started;
    int running;
    int timer_pending;
    int start_error;
    int timer_error;
    int dirs;
    int enum_previous_code;
    const char *enum_path;
    Inventory inventory;
    Row rows[MAX_ROWS];
    Row read;
    int read_attempted;
} App;

static unsigned module_add_ref(Module *module);
static unsigned module_release(Module *module);
static void tick(void *context);

static void copy_string(char *out, const char *in, size_t capacity)
{
    size_t i = 0;
    while (i + 1 < capacity && in[i]) {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

static int same_string(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int file_error(BrewObject *files)
{
    return ((int (*)(BrewObject *))files->vt[FILE_ERROR_SLOT])(files);
}

static int enum_init(BrewObject *files, const char *path, uint8_t dirs)
{
    return ((int (*)(BrewObject *, const char *, uint8_t))files->vt[ENUM_INIT_SLOT])(
        files, path, dirs);
}

static ProbeResult begin_list(void *context, const char *path)
{
    App *app = context;
    app->enum_path = path;
    app->enum_previous_code = 0;
    app->dirs = 0;
    int code = enum_init(app->files, path, 0);
    return (ProbeResult){code ? PROBE_ERROR : PROBE_OK, code};
}

static const char *entry_name(const char *name, const char *path)
{
    /* A enumeração pode devolver um nome simples ou um caminho completo. */
    const char *p = name;
    while (*path && *p == *path) {
        ++p;
        ++path;
    }
    if (*path == '/' && !path[1] && !*p)
        return "";
    if (*path || p == name)
        return name;
    if (p[-1] == '/')
        return p;
    return *p == '/' ? p + 1 : name;
}

static ProbeResult next_entry(void *context, Entry *entry)
{
    App *app = context;
    BrewObject *files = app->files;
    BrewFileInfo info;
    info.name[0] = '\0';
    info.name[sizeof(info.name) - 1] = '\0';
    uint8_t found = ((uint8_t (*)(BrewObject *, BrewFileInfo *))files->vt[ENUM_NEXT_SLOT])(
        files, &info);
    if (found) {
        if (info.name[sizeof(info.name) - 1])
            return (ProbeResult){PROBE_ERROR, BREW_BAD_PARAM};
        copy_string(entry->name, entry_name(info.name, app->enum_path), sizeof(entry->name));
        entry->size = info.size;
        entry->directory = (info.attributes & ATTR_DIRECTORY) != 0;
        return (ProbeResult){PROBE_OK, 0};
    }

    int code = file_error(files);
    if (code && code != BREW_FAILED)
        return (ProbeResult){PROBE_ERROR, code};
    if (app->dirs)
        return (ProbeResult){PROBE_END, code ? code : app->enum_previous_code};

    app->enum_previous_code = code;
    app->dirs = 1;
    code = enum_init(files, app->enum_path, 1);
    if (code)
        return (ProbeResult){PROBE_ERROR, code};
    return next_entry(app, entry);
}

static void end_list(void *context)
{
    /* IFileMgr guarda o cursor. EnumInit substitui o cursor anterior. */
    ((App *)context)->enum_path = NULL;
}

static ProbeResult read_prefix(void *context, const char *path, size_t *bytes)
{
    BrewObject *files = ((App *)context)->files;
    BrewObject *file = ((BrewObject *(*)(BrewObject *, const char *, unsigned))files->vt[FILE_OPEN_SLOT])(
        files, path, FILE_READ_ONLY);
    if (!file)
        return (ProbeResult){PROBE_ERROR, file_error(files)};

    uint8_t buffer[READ_BYTES];
    int count = ((int (*)(BrewObject *, void *, int))file->vt[FILE_READ_SLOT])(
        file, buffer, sizeof(buffer));
    brew_ref(file, RELEASE_SLOT);
    if (count < 0 || count > READ_BYTES)
        return (ProbeResult){PROBE_ERROR, count};
    *bytes = (size_t)count;
    return (ProbeResult){PROBE_OK, 0};
}

static void save_row(Row *row, const Event *event)
{
    copy_string(row->path, event->path, sizeof(row->path));
    row->result = event->result;
    row->kind = event->kind;
    row->count = event->bytes;
}

static void report(void *context, const Event *event)
{
    App *app = context;
    if (event->kind == ENTRY_RESULT) {
        if (event->result.status == PROBE_OK)
            ++app->entry_count;
        return;
    }
    if (event->kind == READ_RESULT) {
        save_row(&app->read, event);
        app->read_attempted = 1;
        return;
    }
    for (unsigned i = 0; i < app->row_count; ++i) {
        if (same_string(app->rows[i].path, event->path)) {
            save_row(&app->rows[i], event);
            return;
        }
    }
    if (app->row_count < MAX_ROWS)
        save_row(&app->rows[app->row_count++], event);
    else
        ++app->omitted_rows;
}

static void draw_text(App *app, int y, const char *text)
{
    uint16_t wide[76];
    unsigned i = 0;
    while (i + 1 < sizeof(wide) / sizeof(wide[0]) && text[i]) {
        wide[i] = (uint8_t)text[i];
        ++i;
    }
    wide[i] = 0;
    BrewObject *display = app->display;
    ((void (*)(BrewObject *, int, const uint16_t *, int, int, int, const void *, uint32_t))
        display->vt[DRAW_TEXT_SLOT])(display, FONT_NORMAL, wide, (int)i, 22, y, NULL, 0);
}

static unsigned append_text(char *out, unsigned offset, const char *text, unsigned max)
{
    while (offset < max && *text)
        out[offset++] = *text++;
    out[offset] = '\0';
    return offset;
}

static unsigned append_hex(char *out, unsigned offset, uint32_t value)
{
    const char *digits = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        out[offset++] = digits[(value >> shift) & 15];
    out[offset] = '\0';
    return offset;
}

static unsigned append_decimal(char *out, unsigned offset, uint32_t value)
{
    /* As contagens exibidas têm no máximo quatro dígitos. */
    uint32_t place = 1000;
    int shown = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned digit = 0;
        while (value >= place) {
            value -= place;
            ++digit;
        }
        if (digit || shown || i == 3) {
            out[offset++] = (char)('0' + digit);
            shown = 1;
        }
        place = i == 0 ? 100 : i == 1 ? 10 : 1;
    }
    out[offset] = '\0';
    return offset;
}

static void draw_row(App *app, int y, const Row *row)
{
    char line[76];
    const char *label = "ERRO";
    if (row->result.status == PROBE_OK)
        label = row->kind == READ_RESULT ? "LEITURA" : "ABERTO";
    else if (row->result.status == PROBE_END)
        label = "FIM";
    else if (row->result.status == PROBE_LIMIT)
        label = "LIMITE";
    unsigned at = append_text(line, 0, label, 9);
    at = append_text(line, at, " 0x", 12);
    at = append_hex(line, at, (uint32_t)row->result.code);
    at = append_text(line, at, " ", 25);
    if ((row->kind == READ_RESULT && row->result.status == PROBE_OK)
        || row->result.status == PROBE_END) {
        at = append_decimal(line, at, (uint32_t)row->count);
        at = append_text(line, at, row->kind == READ_RESULT ? " bytes " : " itens ", 38);
    }
    append_text(line, at, row->path, sizeof(line) - 1);
    draw_text(app, y, line);
}

static void render(App *app)
{
    BrewObject *display = app->display;
    ((void (*)(BrewObject *, const void *, uint32_t, uint32_t, uint32_t))
        display->vt[DRAW_RECT_SLOT])(display, NULL, UINT32_MAX, UINT32_MAX, RECT_FILL);
    draw_text(app, 18, "Inventario Zeebo | Somente leitura");
    if (app->start_error || app->timer_error) {
        char line[76];
        unsigned at = append_text(line, 0, app->start_error ? "IFileMgr: 0x" : "Timer: 0x", 24);
        append_hex(line, at, (uint32_t)(app->start_error ? app->start_error : app->timer_error));
        draw_text(app, 40, line);
    } else if (!app->inventory.finished) {
        draw_text(app, 40, "Enumerando pastas...");
    } else {
        char summary[76];
        unsigned at = append_text(summary, 0, "Entradas observadas: ", 24);
        at = append_decimal(summary, at, app->entry_count);
        if (app->omitted_rows)
            append_text(summary, at, " | Lista parcial", sizeof(summary) - 1);
        draw_text(app, 40, summary);
    }
    if (app->read_attempted)
        draw_row(app, 60, &app->read);
    for (unsigned i = 0; i < app->row_count; ++i)
        draw_row(app, 80 + (int)i * 15, &app->rows[i]);
    ((void (*)(BrewObject *))display->vt[UPDATE_SLOT])(display);
}

static void schedule(App *app)
{
    BrewObject *shell = app->shell;
    int code = ((int (*)(BrewObject *, int, BrewTimer, void *))shell->vt[SET_TIMER_SLOT])(
        shell, 10, tick, app);
    if (code) {
        app->timer_error = code;
        inventory_cancel(&app->inventory);
        render(app);
    } else {
        app->timer_pending = 1;
    }
}

static void stop_timer(App *app)
{
    if (app->timer_pending) {
        BrewObject *shell = app->shell;
        ((void (*)(BrewObject *, BrewTimer, void *))shell->vt[CANCEL_TIMER_SLOT])(shell, tick, app);
        app->timer_pending = 0;
    }
}

static void tick(void *context)
{
    App *app = context;
    app->timer_pending = 0;
    if (!app->running)
        return;
    for (unsigned i = 0; i < STEPS_PER_TICK && inventory_step(&app->inventory); ++i) {
    }
    if (!app->inventory.finished)
        schedule(app);
    else
        render(app);
}

static uint8_t handle_event(App *app, uint16_t event, uint16_t key, uint32_t data)
{
    (void)key;
    (void)data;
    switch (event) {
    case APP_START: {
        if (app->scan_started || app->start_error)
            return 1;
        app->running = 1;
        int code = brew_create(app->shell, FILEMGR_CLASS, &app->files);
        if (!code && app->files) {
            ReadOnlyFs fs = {app, begin_list, next_entry, end_list, read_prefix};
            app->scan_started = inventory_start(&app->inventory, fs, report, app,
                                                STRING_FROM(GAME_ID), NULL);
        }
        if (!app->scan_started)
            app->start_error = code ? code : BREW_FAILED;
        render(app);
        if (app->scan_started)
            schedule(app);
        return 1;
    }
    case APP_STOP:
        app->running = 0;
        stop_timer(app);
        if (app->scan_started)
            inventory_cancel(&app->inventory);
        return 1;
    case APP_SUSPEND:
        app->running = 0;
        stop_timer(app);
        return 1;
    case APP_RESUME:
        app->running = 1;
        render(app);
        if (app->scan_started && !app->inventory.finished && !app->timer_pending)
            schedule(app);
        return 1;
    default:
        return 0;
    }
}

static unsigned app_add_ref(App *app)
{
    return ++app->refs;
}

static unsigned app_release(App *app)
{
    if (--app->refs)
        return app->refs;
    stop_timer(app);
    if (app->scan_started)
        inventory_cancel(&app->inventory);
    if (app->files)
        brew_ref(app->files, RELEASE_SLOT);
    brew_ref(app->display, RELEASE_SLOT);
    brew_ref(app->shell, RELEASE_SLOT);
    Module *module = app->module;
    module->free(app);
    module_release(module);
    return 0;
}

static int create_applet(Module *module, BrewObject *shell, uint32_t class_id, App **out)
{
    if (!out || !shell)
        return BREW_BAD_PARAM;
    *out = NULL;
    if (class_id != INVENTORY_CLASS)
        return BREW_FAILED;
    App *app = module->allocate(sizeof(*app));
    if (!app)
        return BREW_NO_MEMORY;
    for (size_t i = 0; i < sizeof(*app); ++i)
        ((volatile uint8_t *)app)[i] = 0;
    int code = brew_create(shell, DISPLAY_CLASS, &app->display);
    if (code || !app->display) {
        module->free(app);
        return code ? code : BREW_FAILED;
    }
    app->table[0] = (BrewSlot)app_add_ref;
    app->table[1] = (BrewSlot)app_release;
    app->table[2] = (BrewSlot)handle_event;
    app->object.vt = app->table;
    app->module = module;
    app->shell = shell;
    app->refs = 1;
    brew_ref(shell, ADD_REF_SLOT);
    module_add_ref(module);
    *out = app;
    return 0;
}

static unsigned module_add_ref(Module *module)
{
    return ++module->refs;
}

static unsigned module_release(Module *module)
{
    if (--module->refs)
        return module->refs;
    brew_ref(module->shell, RELEASE_SLOT);
    module->free(module);
    return 0;
}

static void free_resources(Module *module, BrewObject *heap, BrewObject *files)
{
    (void)module;
    (void)heap;
    (void)files;
}

int brew_load(BrewObject *shell, void *reserved, Module **out, const BrewSlot *helpers)
{
    (void)reserved;
    if (!shell || !out || !helpers)
        return BREW_BAD_PARAM;
    *out = NULL;
    Allocate allocate = (Allocate)helpers[HELP_ALLOC_SLOT];
    Deallocate deallocate = (Deallocate)helpers[HELP_FREE_SLOT];
    if (!allocate || !deallocate)
        return BREW_FAILED;
    Module *module = allocate(sizeof(*module));
    if (!module)
        return BREW_NO_MEMORY;
    module->table[0] = (BrewSlot)module_add_ref;
    module->table[1] = (BrewSlot)module_release;
    module->table[2] = (BrewSlot)create_applet;
    module->table[3] = (BrewSlot)free_resources;
    module->object.vt = module->table;
    module->shell = shell;
    module->allocate = allocate;
    module->free = deallocate;
    module->refs = 1;
    brew_ref(shell, ADD_REF_SLOT);
    *out = module;
    return 0;
}
