#include "inventario.h"

static int append(char *path, size_t *length, const char *suffix)
{
    while (*suffix) {
        if (*length >= PATH_CAPACITY - 1)
            return 0;
        path[(*length)++] = *suffix++;
    }
    path[*length] = '\0';
    return 1;
}

static int join(char *out, const char *parent, const char *name)
{
    size_t length = 0;
    return append(out, &length, parent)
        && append(out, &length, (length && out[length - 1] != '/') ? "/" : "")
        && append(out, &length, name);
}

static int valid_name(const char *name)
{
    if (!name[0] || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

static int same_name(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static void emit(Inventory *inventory, EventKind kind, const char *path, ProbeResult result,
                 unsigned long bytes, int directory)
{
    Event event = {kind, path, result, bytes, directory};
    inventory->report(inventory->report_context, &event);
}

static int queue_directory(Inventory *inventory, const char *parent, const char *name,
                           unsigned depth)
{
    if (inventory->directory_count == MAX_DIRECTORIES || depth > MAX_DEPTH)
        return 0;
    Directory *dir = &inventory->pending[inventory->directory_count];
    if (!join(dir->path, parent, name))
        return 0;
    dir->depth = depth;
    ++inventory->directory_count;
    return 1;
}

int inventory_start(Inventory *inventory, ReadOnlyFs fs, Report report, void *context,
                    const char *game_id, const char *sample_name)
{
    size_t id_length = 0;
    while (game_id[id_length] >= '0' && game_id[id_length] <= '9')
        ++id_length;
    if (id_length != 6 || game_id[id_length] || (sample_name && !valid_name(sample_name)))
        return 0;

    inventory->fs = fs;
    inventory->report = report;
    inventory->report_context = context;
    inventory->next_directory = 0;
    inventory->directory_count = 0;
    inventory->entries = 0;
    inventory->listing = 0;
    inventory->finished = 0;
    inventory->read_path[0] = '\0';
    if (!queue_directory(inventory, "fs:/", "mod/", 0)
        || !queue_directory(inventory, "fs:/", "mif/", 0)
        || !queue_directory(inventory, "fs:/", "card0/", 0)
        || !queue_directory(inventory, "fs:/mod", game_id, 1))
        return 0;

    const char *game_path = inventory->pending[inventory->directory_count - 1].path;
    if (!queue_directory(inventory, game_path, "udata", 2))
        return 0;
    const char *save_path = inventory->pending[inventory->directory_count - 1].path;
    if (sample_name)
        return join(inventory->read_path, save_path, sample_name);

    size_t length = 0;
    return append(inventory->read_path, &length, "fs:/mif/")
        && append(inventory->read_path, &length, game_id)
        && append(inventory->read_path, &length, ".mif");
}

static void advance(Inventory *inventory)
{
    if (inventory->listing)
        inventory->fs.end_list(inventory->fs.context);
    inventory->listing = 0;
    ++inventory->next_directory;
}

int inventory_step(Inventory *inventory)
{
    if (inventory->finished)
        return 0;
    ReadOnlyFs *fs = &inventory->fs;
    if (inventory->next_directory == inventory->directory_count) {
        if (inventory->read_path[0]) {
            size_t bytes = 0;
            ProbeResult result = fs->read_prefix(fs->context, inventory->read_path, &bytes);
            emit(inventory, READ_RESULT, inventory->read_path, result, bytes, 0);
        }
        inventory->finished = 1;
        return 0;
    }

    Directory *dir = &inventory->pending[inventory->next_directory];
    if (!inventory->listing) {
        ProbeResult result = fs->begin_list(fs->context, dir->path);
        emit(inventory, LIST_RESULT, dir->path, result, 0, 1);
        if (result.status == PROBE_OK) {
            inventory->listing = 1;
            inventory->entries = 0;
        } else {
            advance(inventory);
        }
        return 1;
    }

    Entry entry;
    entry.name[0] = '\0';
    entry.name[PATH_CAPACITY - 1] = '\0';
    ProbeResult result = fs->next_entry(fs->context, &entry);
    if (result.status != PROBE_OK) {
        emit(inventory, LIST_RESULT, dir->path, result, inventory->entries, 1);
        advance(inventory);
        return 1;
    }
    if (inventory->entries == MAX_ENTRIES) {
        emit(inventory, LIST_RESULT, dir->path, (ProbeResult){PROBE_LIMIT, 0},
             inventory->entries, 1);
        advance(inventory);
        return 1;
    }

    ++inventory->entries;
    char path[PATH_CAPACITY];
    if (entry.name[PATH_CAPACITY - 1] || !valid_name(entry.name)
        || !join(path, dir->path, entry.name)) {
        emit(inventory, ENTRY_RESULT, dir->path, (ProbeResult){PROBE_SKIPPED, 0}, 0, 0);
        return 1;
    }
    emit(inventory, ENTRY_RESULT, path, result, entry.size, entry.directory);
    if (entry.directory && dir->depth > 0 && !(dir->depth == 1 && same_name(entry.name, "udata"))
        && !queue_directory(inventory, dir->path, entry.name, dir->depth + 1)) {
        emit(inventory, LIST_RESULT, path, (ProbeResult){PROBE_LIMIT, 0}, 0, 1);
    }
    return 1;
}

void inventory_cancel(Inventory *inventory)
{
    if (inventory->listing)
        advance(inventory);
    inventory->finished = 1;
}
