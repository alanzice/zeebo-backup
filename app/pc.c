#include "inventario.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *path;
    ProbeResult result;
    const Entry *entries;
    size_t count;
} FixtureDirectory;

typedef struct {
    const FixtureDirectory *active;
    size_t next;
    int restricted;
    int truncated;
} Fixture;

static const Entry modules[] = {{"123456", 0, 1}, {"654321", 0, 1}};
static const Entry descriptors[] = {{"123456.mif", 128, 0}};
static const Entry game[] = {{"123456.mod", 4096, 0}, {"udata", 0, 1}, {"assets", 0, 1}};
static const char sample[] = "save ficticio\n";
static const Entry saves[] = {{"amostra.dat", sizeof(sample) - 1, 0}, {"slot2", 0, 1}};
static const Entry slot2[] = {{"save.dat", 42, 0}, {"..", 0, 1}};
static const Entry assets[] = {{"video.bar", 1024, 0}};

#define DIRECTORY(path, items) {path, {PROBE_OK, 0}, items, sizeof(items) / sizeof(items[0])}
static const FixtureDirectory directories[] = {
    DIRECTORY("fs:/mod/", modules),
    DIRECTORY("fs:/mif/", descriptors),
    {"fs:/card0/", {PROBE_OK, 0}, NULL, 0},
    DIRECTORY("fs:/mod/123456", game),
    DIRECTORY("fs:/mod/123456/udata", saves),
    DIRECTORY("fs:/mod/123456/udata/slot2", slot2),
    DIRECTORY("fs:/mod/123456/assets", assets),
};

static ProbeResult begin_list(void *context, const char *path)
{
    Fixture *fixture = context;
    if (fixture->active)
        return (ProbeResult){PROBE_ERROR, 16};
    if (fixture->restricted && strcmp(path, "fs:/mif/") == 0)
        return (ProbeResult){PROBE_DENIED, 5};
    if (fixture->restricted && strcmp(path, "fs:/card0/") == 0)
        return (ProbeResult){PROBE_NO_MEDIA, 21};
    if (fixture->restricted && strcmp(path, "fs:/mod/123456/assets") == 0)
        return (ProbeResult){PROBE_MISSING, 3};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        if (strcmp(path, directories[i].path) == 0) {
            fixture->active = &directories[i];
            fixture->next = 0;
            return directories[i].result;
        }
    }
    return (ProbeResult){PROBE_MISSING, 3};
}

static ProbeResult next_entry(void *context, Entry *entry)
{
    Fixture *fixture = context;
    const FixtureDirectory *dir = fixture->active;
    if (fixture->restricted && strcmp(dir->path, "fs:/mod/123456/udata/slot2") == 0
        && fixture->next == 1)
        return (ProbeResult){PROBE_ERROR, 23};
    if (fixture->truncated && strcmp(dir->path, "fs:/mod/") == 0) {
        ++fixture->next;
        *entry = modules[0];
        return (ProbeResult){PROBE_OK, 0};
    }
    if (fixture->next == dir->count)
        return (ProbeResult){PROBE_END, 0};
    *entry = dir->entries[fixture->next++];
    return (ProbeResult){PROBE_OK, 0};
}

static void end_list(void *context)
{
    Fixture *fixture = context;
    fixture->active = NULL;
}

static ProbeResult read_prefix(void *context, const char *path, size_t *bytes)
{
    const Fixture *fixture = context;
    if (fixture->active)
        return (ProbeResult){PROBE_ERROR, 16};
    if (strcmp(path, "fs:/mod/123456/udata/amostra.dat") != 0)
        return (ProbeResult){PROBE_MISSING, 2};
    *bytes = strlen(sample);
    return (ProbeResult){PROBE_OK, 0};
}

static void report(void *context, const Event *event)
{
    (void)context;
    static const char *const status[] = {
        "ok", "fim", "ausente", "acesso negado", "midia indisponivel",
        "erro", "limite", "entrada ignorada"
    };
    const char *action = event->kind == LIST_RESULT ? "listar" :
        event->kind == READ_RESULT ? "ler" : event->directory ? "pasta" : "arquivo";
    printf("%-8s %-40s %s (codigo %d)", action, event->path,
           status[event->result.status], event->result.code);
    if (event->kind == READ_RESULT || (event->kind == ENTRY_RESULT && !event->directory
                                      && event->result.status == PROBE_OK))
        printf(", %lu bytes", event->bytes);
    if (event->kind == LIST_RESULT && event->result.status == PROBE_END)
        printf(", %lu entradas", event->bytes);
    puts("");
}

int main(int argc, char **argv)
{
    Fixture fixture = {0};
    int cancel = argc == 2 && strcmp(argv[1], "--cancelar") == 0;
    if (argc == 2) {
        fixture.restricted = strcmp(argv[1], "--restrito") == 0;
        fixture.truncated = strcmp(argv[1], "--limite") == 0;
    }
    if (argc > 2 || (argc == 2 && !cancel && !fixture.restricted && !fixture.truncated)) {
        fputs("Uso: inventario-pc [--restrito|--limite|--cancelar]\n", stderr);
        return 2;
    }

    ReadOnlyFs fs = {&fixture, begin_list, next_entry, end_list, read_prefix};
    Inventory inventory;
    puts("Inventario Zeebo | Dados ficticios em memoria");
    if (!inventory_start(&inventory, fs, report, NULL, "123456", "amostra.dat"))
        return 1;
    if (cancel) {
        inventory_step(&inventory);
        inventory_cancel(&inventory);
        if (fixture.active || inventory_step(&inventory))
            return 1;
        puts("Inventario cancelado");
        return 0;
    }
    while (inventory_step(&inventory)) {
    }
    return fixture.active ? 1 : 0;
}
