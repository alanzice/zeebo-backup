#include "tela.h"

#ifndef INVENTORY_CLASS
#error Defina a ClassID ao compilar.
#endif

enum { KEY_EVENT = 0x0100, KEY_PRESS = 0x0101, KEY_RELEASE = 0x0102,
       KEY_HOME = 0xe030, KEY_DIGIT_TWO = 0xe023, KEY_UP = 0xe031, KEY_DOWN = 0xe032,
       KEY_LEFT = 0xe033, KEY_RIGHT = 0xe034, KEY_SELECT = 0xe035,
       KEY_PAD_BACK = 0xe04a, KEY_PAD_SELECT = 0xe064, TICK_MS = 15, STEPS = 5, SCAN_STEPS = 20,
       HOME_INTERVAL_MS = 1600, PAD_INTERVAL_MS = 30, PAD_COUNT = 2,
       HID_CLASS = 0x0106c411, HID_JOYSTICK = 0x0106c3fd, HID_CREATE_DEVICE = 3,
       HID_GET_CONNECTED_DEVICES = 7, HID_GET_BUTTON_INFO = 6, HID_GET_BUTTON_COUNT = 7,
       PAD_BUTTON_TWO = 1, RIGHT_SHOULDER = 0x0106c408 };

typedef void *(*Alloc)(size_t);
typedef void (*Free)(void *);

typedef struct {
    BrewObject *device;
    int back_down;
    int theme_button;
    int theme_down;
} Pad;

// AEEHIDButtonInfo do SDK.
typedef struct { int id, state, uid, min, max; } ButtonInfo;

typedef struct {
    BrewObject object;
    BrewSlot table[4];
    BrewObject *shell;
    Alloc allocate;
    Free free;
    uint32_t (*uptime)(void);
    unsigned refs;
} Module;

typedef struct {
    BrewObject object;
    BrewSlot table[3];
    Module *module;
    BrewObject *shell;
    BrewObject *display;
    BrewObject *files;
    unsigned refs;
    unsigned redraw;
    uint32_t copy_started_at, copy_before_pause;
    int timer_pending;
    int pad_timer_pending;
    Pad pads[PAD_COUNT];
    View view;
    Catalog catalog;
    Backup backup;
} App;

static void tick(void *context);
static void pad_tick(void *context);
static void reset_home(void *context);
static void clear_home(App *app);

static void update_running_clock(App *app)
{
    if (!app->module->uptime) {
        ++app->view.activity_frame;
        return;
    }
    uint32_t now = app->module->uptime();
    app->view.activity_frame = now / 180;
    if (app->backup.include_config && !app->backup.planning && !app->backup.reading_checkpoint)
        app->view.copy_elapsed_ms = app->copy_before_pause + (now - app->copy_started_at);
}

static void render(App *app)
{
    if (app->view.screen == RUNNING && !app->view.paused)
        update_running_clock(app);
    if (app->view.screen == GAMES) {
        unsigned first = app->view.first_game;
        for (unsigned i = first; i < app->catalog.count && i < first + GAME_ROWS; ++i)
            catalog_load_title(app->files, &app->catalog, i);
    }
    render_backup(app->display, &app->view, &app->catalog, &app->backup);
}

static void clear_timer(App *app, BrewTimer callback)
{
    BrewObject *shell = app->shell;
    ((void (*)(BrewObject *, BrewTimer, void *))shell->vt[CANCEL_TIMER_SLOT])(shell, callback, app);
}

static int set_timer(App *app, int delay, BrewTimer callback)
{
    BrewObject *shell = app->shell;
    return ((int (*)(BrewObject *, int, BrewTimer, void *))shell->vt[SET_TIMER_SLOT])(
        shell, delay, callback, app);
}

static void release_checkpoint_index(App *app)
{
    if (app->backup.resume_index) {
        app->module->free(app->backup.resume_index);
        app->backup.resume_index = NULL;
    }
}

static void cancel_timer(App *app)
{
    if (app->timer_pending) {
        clear_timer(app, tick);
        app->timer_pending = 0;
    }
}

static void schedule(App *app)
{
    int code = set_timer(app, TICK_MS, tick);
    if (!code) {
        app->timer_pending = 1;
    } else {
        app->view.timer_error = code;
        backup_cancel(&app->backup);
        release_checkpoint_index(app);
        app->view.screen = RESULTS;
        render(app);
    }
}

static void tick(void *context)
{
    App *app = context;
    app->timer_pending = 0;
    if (app->view.screen != RUNNING)
        return;
    int planning = app->backup.planning || app->backup.reading_checkpoint;
    unsigned steps = planning ? SCAN_STEPS : STEPS;
    for (unsigned i = 0; i < steps && app->backup.state == BACKUP_RUNNING; ++i) {
        backup_step(&app->backup);
        if (planning && !app->backup.planning && !app->backup.reading_checkpoint)
            break;
    }
    if (app->backup.state == BACKUP_RUNNING) {
        const int copying = planning && !app->backup.planning && !app->backup.reading_checkpoint;
        if (copying && app->module->uptime)
            app->copy_started_at = app->module->uptime();
        if (copying || ++app->redraw == 8) {
            app->redraw = 0;
            render(app);
        }
        schedule(app);
    } else {
        release_checkpoint_index(app);
        app->view.screen = RESULTS;
        render(app);
    }
}

static void select_scope(App *app, int selected)
{
    app->view.resume_path[0] = '\0';
    app->view.selected_game = selected;
    app->view.confirm_copy = 0;
    app->view.sd_space_known = !sd_free_space(app->files, &app->view.sd_available);
    app->view.screen = CONFIRM;
}

static void start_backup(App *app)
{
    const char *resume = app->view.resume_path[0] ? app->view.resume_path : NULL;
    uint32_t *index = NULL;
    if (resume) {
        index = app->module->allocate(RESUME_SLOTS * sizeof(*index) + app->view.resume_size);
        if (!index) {
            app->view.resume_error = BREW_NO_MEMORY;
            return;
        }
        for (unsigned i = 0; i < RESUME_SLOTS; ++i)
            ((volatile uint32_t *)index)[i] = 0;
    }
    app->view.timer_error = 0;
    app->view.paused = 0;
    app->view.copy_elapsed_ms = app->copy_before_pause = 0;
    app->redraw = 0;
    backup_start(&app->backup, app->shell, app->files, &app->catalog, app->view.selected_game,
                 resume, index, app->view.resume_size);
    app->view.screen = app->backup.state == BACKUP_RUNNING ? RUNNING : RESULTS;
    if (app->view.screen == RUNNING)
        schedule(app);
    else
        release_checkpoint_index(app);
}

static void stop_backup(App *app)
{
    cancel_timer(app);
    backup_cancel(&app->backup);
    release_checkpoint_index(app);
    app->view.paused = 0;
    if (app->view.screen == RUNNING)
        app->view.screen = RESULTS;
}

static uint8_t handle_key(App *app, unsigned key)
{
    View *view = &app->view;
    int select = key == KEY_SELECT || key == KEY_PAD_SELECT;
    int back = key == KEY_DIGIT_TWO || key == KEY_PAD_BACK;
    switch (view->screen) {
    case HOME:
        if (key == KEY_UP) {
            if (view->home_choice > HOME_FULL)
                --view->home_choice;
        } else if (key == KEY_DOWN) {
            if (view->home_choice < HOME_RESUME)
                ++view->home_choice;
        } else if (select) {
            if (view->home_choice == HOME_GAME) {
                catalog_load(app->files, &app->catalog);
                view->screen = GAMES;
                view->game_choice = view->first_game = 0;
            } else if (view->home_choice == HOME_HELP) {
                view->screen = INSTRUCTIONS;
            } else if (view->home_choice == HOME_RESUME) {
                view->selected_game = -1;
                view->resume_error = 0;
                view->resume_path[0] = '\0';
                view->resume_size = backup_find_resume(app->files, view->resume_path);
                view->screen = RESUME_BACKUP;
            } else {
                select_scope(app, -1);
            }
        } else {
            return 0;
        }
        break;
    case GAMES:
        if (back || key == KEY_LEFT) {
            view->screen = HOME;
        } else if (key == KEY_UP && view->game_choice) {
            if (--view->game_choice < view->first_game)
                view->first_game = view->game_choice;
        } else if (key == KEY_DOWN && view->game_choice + 1 < app->catalog.count) {
            if (++view->game_choice >= view->first_game + GAME_ROWS)
                ++view->first_game;
        } else if (select && app->catalog.count) {
            select_scope(app, (int)view->game_choice);
        } else {
            return key == KEY_UP || key == KEY_DOWN || select;
        }
        break;
    case CONFIRM:
        if (key == KEY_RIGHT)
            view->confirm_copy = 1;
        else if (key == KEY_LEFT)
            view->confirm_copy = 0;
        else if (back || (select && !view->confirm_copy))
            view->screen = view->selected_game < 0 ? HOME : GAMES;
        else if (select)
            start_backup(app);
        else
            return 0;
        break;
    case RUNNING:
        if (back) {
            stop_backup(app);
        } else if (select && app->backup.include_config) {
            if (!view->paused) {
                update_running_clock(app);
                app->copy_before_pause = view->copy_elapsed_ms;
                view->paused = 1;
                cancel_timer(app);
            } else {
                view->paused = 0;
                if (app->module->uptime)
                    app->copy_started_at = app->module->uptime();
                schedule(app);
            }
        } else {
            return 0;
        }
        break;
    case RESUME_BACKUP:
        if (back || key == KEY_LEFT)
            view->screen = HOME;
        else if (select && view->resume_path[0])
            start_backup(app);
        else if (!select)
            return 0;
        break;
    case RESULTS:
        if (key == KEY_RIGHT && app->backup.sd_setup_failed)
            view->screen = SD_DETAILS;
        else if (select || back || key == KEY_LEFT)
            view->screen = HOME;
        else
            return 0;
        break;
    case SD_DETAILS:
        if (!select && !back && key != KEY_LEFT)
            return 0;
        view->screen = RESULTS;
        break;
    case INSTRUCTIONS:
        if (!select && !back && key != KEY_LEFT)
            return 0;
        view->screen = HOME;
        break;
    default:
        return 0;
    }
    render(app);
    return 1;
}

static void stop_pads(App *app)
{
    if (app->pad_timer_pending) {
        clear_timer(app, pad_tick);
        app->pad_timer_pending = 0;
    }
    for (unsigned i = 0; i < PAD_COUNT; ++i) {
        Pad *pad = &app->pads[i];
        if (pad->device)
            brew_ref(pad->device, RELEASE_SLOT);
        pad->device = NULL;
        pad->back_down = 0;
        pad->theme_button = -1;
        pad->theme_down = 0;
    }
}

static int get_button(BrewObject *device, int index, ButtonInfo *button)
{
    return ((int (*)(BrewObject *, int, ButtonInfo *))device->vt[HID_GET_BUTTON_INFO])(
        device, index, button);
}

static void select_theme_button(Pad *pad)
{
    BrewObject *device = pad->device;
    int count = 0;
    pad->theme_button = -1;
    if (((int (*)(BrewObject *, int *))device->vt[HID_GET_BUTTON_COUNT])(device, &count))
        return;
    for (int index = 0; index < count && index < 64; ++index) {
        ButtonInfo button;
        if (!get_button(device, index, &button) && button.uid == RIGHT_SHOULDER) {
            pad->theme_button = index;
            pad->theme_down = button.state > button.min;
            return;
        }
    }
}

static void pad_tick(void *context)
{
    App *app = context;
    app->pad_timer_pending = 0;
    for (unsigned i = 0; i < PAD_COUNT; ++i) {
        Pad *pad = &app->pads[i];
        if (!pad->device)
            continue;
        ButtonInfo button;
        if (!get_button(pad->device, PAD_BUTTON_TWO, &button)) {
            int down = button.state > button.min;
            if (down && !pad->back_down) {
                int had_home_taps = app->view.home_taps != 0;
                clear_home(app);
                if (!handle_key(app, KEY_DIGIT_TWO) && had_home_taps)
                    render(app);
            }
            pad->back_down = down;
        }
        if (pad->theme_button >= 0 && !get_button(pad->device, pad->theme_button, &button)) {
            int down = button.state > button.min;
            if (down && !pad->theme_down) {
                clear_home(app);
                app->view.dark_theme = !app->view.dark_theme;
                render(app);
            }
            pad->theme_down = down;
        }
    }
    if (!set_timer(app, PAD_INTERVAL_MS, pad_tick))
        app->pad_timer_pending = 1;
    else
        stop_pads(app);
}

static void start_pads(App *app)
{
    BrewObject *hid = NULL;
    if (app->pad_timer_pending || brew_create(app->shell, HID_CLASS, &hid) || !hid)
        return;
    int handles[PAD_COUNT];
    int count = 0;
    int code = ((int (*)(BrewObject *, int, int *, int, int *))hid->vt[HID_GET_CONNECTED_DEVICES])(
        hid, HID_JOYSTICK, handles, PAD_COUNT, &count);
    if (!code) {
        for (int i = 0; i < count && i < PAD_COUNT; ++i) {
            ((int (*)(BrewObject *, int, BrewObject **))hid->vt[HID_CREATE_DEVICE])(
                hid, handles[i], &app->pads[i].device);
            if (app->pads[i].device)
                select_theme_button(&app->pads[i]);
        }
    }
    brew_ref(hid, RELEASE_SLOT);
    if (app->pads[0].device || app->pads[1].device)
        pad_tick(app);
}

static void reset_home(void *context)
{
    App *app = context;
    app->view.home_taps = 0;
    render(app);
}

static void clear_home(App *app)
{
    if (app->view.home_taps) {
        clear_timer(app, reset_home);
        app->view.home_taps = 0;
    }
}

static void home_tap(App *app)
{
    unsigned count = app->view.home_taps;
    clear_home(app);
    if (count == 2) {
        stop_backup(app);
        stop_pads(app);
        BrewObject *shell = app->shell;
        // FALSE deixa o BREW retomar o applet que estava atrás do atual.
        ((void (*)(BrewObject *, uint8_t))shell->vt[CLOSE_APPLET_SLOT])(shell, 0);
        return;
    }
    if (!set_timer(app, HOME_INTERVAL_MS, reset_home))
        app->view.home_taps = count + 1;
    render(app);
}

static uint8_t handle_event(App *app, uint16_t event, uint16_t key, uint32_t data)
{
    (void)data;
    switch (event) {
    case APP_START:
        if (!app->files) {
            int code = brew_create(app->shell, FILEMGR_CLASS, &app->files);
            if (code || !app->files) {
                app->view.startup_error = code ? code : BREW_FAILED;
                app->view.screen = FILES_UNAVAILABLE;
            } else {
                catalog_load(app->files, &app->catalog);
                app->view.screen = HOME;
            }
        }
        start_pads(app);
        render(app);
        return 1;
    case APP_STOP:
    case APP_SUSPEND:
        stop_backup(app);
        stop_pads(app);
        clear_home(app);
        return 1;
    case APP_RESUME:
        start_pads(app);
        render(app);
        return 1;
    case KEY_PRESS:
        return key == KEY_HOME;
    case KEY_RELEASE:
        if (key != KEY_HOME)
            return 0;
        home_tap(app);
        return 1;
    case KEY_EVENT:
        if (key == KEY_HOME || (key == KEY_PAD_BACK && app->pad_timer_pending))
            return 1;
        if (app->view.home_taps) {
            clear_home(app);
            if (!handle_key(app, key))
                render(app);
            return 1;
        }
        return handle_key(app, key);
    default:
        return 0;
    }
}

static unsigned module_addref(Module *module)
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

static void module_free_resources(Module *module, BrewObject *heap, BrewObject *files)
{
    (void)module;
    (void)heap;
    (void)files;
}

static unsigned app_addref(App *app)
{
    return ++app->refs;
}

static unsigned app_release(App *app)
{
    if (--app->refs)
        return app->refs;
    stop_backup(app);
    stop_pads(app);
    clear_home(app);
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
    if (!shell || !out)
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
    app->table[0] = (BrewSlot)app_addref;
    app->table[1] = (BrewSlot)app_release;
    app->table[2] = (BrewSlot)handle_event;
    app->object.vt = app->table;
    app->shell = shell;
    app->module = module;
    app->view.clock_available = module->uptime != NULL;
    app->refs = 1;
    brew_ref(shell, ADD_REF_SLOT);
    module_addref(module);
    *out = app;
    return 0;
}

int brew_load(BrewObject *shell, void *reserved, Module **out, const BrewSlot *helpers)
{
    (void)reserved;
    if (!shell || !out || !helpers)
        return BREW_BAD_PARAM;
    *out = NULL;
    Alloc allocate = (Alloc)helpers[HELP_ALLOC_SLOT];
    Free free = (Free)helpers[HELP_FREE_SLOT];
    if (!allocate || !free)
        return BREW_FAILED;
    Module *module = allocate(sizeof(*module));
    if (!module)
        return BREW_NO_MEMORY;
    module->table[0] = (BrewSlot)module_addref;
    module->table[1] = (BrewSlot)module_release;
    module->table[2] = (BrewSlot)create_applet;
    module->table[3] = (BrewSlot)module_free_resources;
    module->object.vt = module->table;
    module->shell = shell;
    module->allocate = allocate;
    module->free = free;
    module->uptime = (uint32_t (*)(void))helpers[HELP_UPTIME_SLOT];
    module->refs = 1;
    brew_ref(shell, ADD_REF_SLOT);
    *out = module;
    return 0;
}
