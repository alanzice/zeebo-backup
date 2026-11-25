#include "backup.h"

#define CARD "fs:/card0/"
#define ROOT CARD "ZB"
#define HISTORY ROOT "/STATUS.TXT"
#define FULL_HEADER "Zeebo Backup\r\nESCOPO COMPLETO\r\n"
#define RESUME_MARK "RETOMADA\r\n"
#define JOURNAL_SEEN UINT32_C(0x80000000)

enum { PATH_ERROR = -1, DEPTH_ERROR = -2, INTEGRITY_ERROR = -3,
       CATALOG_ERROR = -5, MARKER_ERROR = -6, SD_SPACE_INVALID = -7,
       SD_SPACE_INSUFFICIENT = -8, CHECKPOINT_ERROR = -9, STALE_ERROR = -10,
       ENUM_LIMIT = 4096, SESSION_LIMIT = 10000, CHECKPOINT_LIMIT = 4 * 1024 * 1024 };

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static const char *after_prefix(const char *value, const char *prefix)
{
    while (*prefix) {
        if (*value++ != *prefix++)
            return NULL;
    }
    return value;
}

static int path_join(char *out, const char *first, const char *second)
{
    unsigned at = text_add(out, 0, first, PATH_SIZE - 1);
    if (!at || first[at] || at == PATH_SIZE - 1)
        return 0;
    if (out[at - 1] != '/')
        at = text_add(out, at, "/", PATH_SIZE - 1);
    unsigned end = text_add(out, at, second, PATH_SIZE - 1);
    return !second[end - at];
}

static int valid_name(const char *name)
{
    if (!*name || same(name, ".") || same(name, ".."))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        unsigned c = *p;
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|' || (!p[1] && (c == '.' || c == ' ')))
            return 0;
    }
    return 1;
}

static unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int equal_case(const char *a, const char *b)
{
    while (*a && lower((uint8_t)*a) == lower((uint8_t)*b)) { ++a; ++b; }
    return lower((uint8_t)*a) == lower((uint8_t)*b);
}

static int excluded_name(const char *name)
{
    const char *extension = "";
    for (const char *p = name; *p; ++p) {
        if (*p == '.')
            extension = p;
    }
    return equal_case(name, "61s.dat") || equal_case(extension, ".key")
        || equal_case(extension, ".blf") || equal_case(extension, ".dk");
}

static int card_path(const char *path)
{
    const char *p = after_prefix(path, ROOT);
    if (!p)
        return 0;
    if (!*p)
        return 1;
    if (*p++ != '/')
        return 0;
    while (*p) {
        char name[PATH_SIZE];
        unsigned n = 0;
        while (*p && *p != '/' && n < sizeof(name) - 1)
            name[n++] = *p++;
        name[n] = '\0';
        if (!valid_name(name) || (*p && *p != '/'))
            return 0;
        if (*p++ == '\0')
            return 1;
    }
    return 0;
}

static int file_operation(BrewObject *files, unsigned slot, const char *path)
{
    return ((int (*)(BrewObject *, const char *))files->vt[slot])(files, path);
}

static int last_file_error(BrewObject *files)
{
    return ((int (*)(BrewObject *))files->vt[FILE_ERROR_SLOT])(files);
}

static int file_error(BrewObject *files)
{
    int error = last_file_error(files);
    return error ? error : BREW_FAILED;
}

static int info(BrewObject *files, const char *path, BrewFileInfo *result)
{
    return ((int (*)(BrewObject *, const char *, BrewFileInfo *))files->vt[FILE_INFO_SLOT])(
        files, path, result);
}

static BrewObject *open_file(BrewObject *files, const char *path, unsigned mode)
{
    if (mode == FILE_CREATE && (!card_path(path) || !file_operation(files, FILE_TEST_SLOT, path)))
        return NULL;
    if (mode == FILE_APPEND && !card_path(path))
        return NULL;
    return ((BrewObject *(*)(BrewObject *, const char *, unsigned))files->vt[FILE_OPEN_SLOT])(
        files, path, mode);
}

static int make_directory(BrewObject *files, const char *path)
{
    return card_path(path) ? file_operation(files, FILE_MKDIR_SLOT, path) : PATH_ERROR;
}

static int ensure_directory(BrewObject *files, const char *path)
{
    BrewFileInfo result;
    int code = info(files, path, &result);
    if (!code)
        return (result.attributes & ATTR_DIRECTORY) ? 0 : BACKUP_DESTINATION_CONFLICT;
    return make_directory(files, path);
}

static void close_object(BrewObject **object)
{
    if (*object) {
        brew_ref(*object, RELEASE_SLOT);
        *object = NULL;
    }
}

static int enum_init(BrewObject *files, const char *path, uint8_t dirs)
{
    return ((int (*)(BrewObject *, const char *, uint8_t))files->vt[ENUM_INIT_SLOT])(
        files, path, dirs);
}

static int enum_next(BrewObject *files, BrewFileInfo *entry)
{
    entry->name[0] = '\0';
    entry->name[sizeof(entry->name) - 1] = '\0';
    return ((uint8_t (*)(BrewObject *, BrewFileInfo *))files->vt[ENUM_NEXT_SLOT])(files, entry);
}

static const char *entry_name(const char *parent, const char *raw, unsigned capacity)
{
    if (raw[capacity - 1])
        return NULL;
    const char *name = after_prefix(raw, parent);
    if (name) {
        const char *end = parent;
        while (*end)
            ++end;
        if (end[-1] != '/') {
            if (*name != '/')
                return NULL;
            ++name;
        }
    } else {
        name = raw;
    }
    return valid_name(name) ? name : NULL;
}

static void mif_path(char *path, const char *id)
{
    unsigned at = text_add(path, 0, "fs:/mif/", PATH_SIZE - 1);
    at = text_add(path, at, id, PATH_SIZE - 1);
    text_add(path, at, ".mif", PATH_SIZE - 1);
}

void catalog_load(BrewObject *files, Catalog *catalog)
{
    catalog->count = 0;
    catalog->omitted = 0;
    catalog->error = enum_init(files, "fs:/mod/", 1);
    if (catalog->error)
        return;

    for (unsigned scanned = 0; scanned < ENUM_LIMIT; ++scanned) {
        BrewFileInfo entry;
        if (!enum_next(files, &entry)) {
            int code = file_error(files);
            catalog->error = code == BREW_FAILED ? 0 : code;
            return;
        }
        const char *name = entry_name("fs:/mod/", entry.name, sizeof(entry.name));
        if (!name || !(entry.attributes & ATTR_DIRECTORY))
            continue;
        unsigned length = 0;
        while (name[length])
            ++length;
        if (length >= GAME_ID_SIZE)
            continue;
        if (!numeric_id(name) || length != 6 || same(name, "274755")) {
            char path[PATH_SIZE];
            mif_path(path, name);
            if (file_operation(files, FILE_TEST_SLOT, path))
                continue;
        }
        unsigned at = 0;
        for (; at < catalog->count; ++at) {
            const char *other = catalog->games[at];
            unsigned digit = 0;
            while (name[digit] && name[digit] == other[digit])
                ++digit;
            if ((uint8_t)name[digit] <= (uint8_t)other[digit])
                break;
        }
        if (at < catalog->count && same(catalog->games[at], name))
            continue;
        if (catalog->count == GAME_COUNT) {
            ++catalog->omitted;
            continue;
        }
        for (unsigned i = catalog->count; i > at; --i)
            text_add(catalog->games[i], 0, catalog->games[i - 1], GAME_ID_SIZE - 1);
        text_add(catalog->games[at], 0, name, GAME_ID_SIZE - 1);
        catalog->title_checked[catalog->count] = 0;
        catalog->titles[catalog->count][0] = '\0';
        ++catalog->count;
    }
    catalog->error = CATALOG_ERROR;
}

static uint32_t write_file(BrewObject *file, const uint8_t *data, uint32_t size)
{
    return ((uint32_t (*)(BrewObject *, const void *, uint32_t))file->vt[FILE_WRITE_SLOT])(
        file, data, size);
}

static int write_all(BrewObject *file, const uint8_t *data, uint32_t size)
{
    while (size) {
        uint32_t wrote = write_file(file, data, size);
        if (!wrote || wrote > size)
            return 0;
        size -= wrote;
        data += wrote;
    }
    return 1;
}

static int read_file(BrewObject *file, uint8_t *data, unsigned size)
{
    return ((int (*)(BrewObject *, void *, unsigned))file->vt[FILE_READ_SLOT])(file, data, size);
}

static uint32_t mif_le(const uint8_t *bytes, unsigned size)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}

static int seek_file(BrewObject *file, uint32_t offset)
{
    return ((int32_t (*)(BrewObject *, int8_t, int32_t))file->vt[FILE_SEEK_SLOT])(
        file, FILE_SEEK_START, (int32_t)offset);
}

static int mif_read(BrewObject *file, uint32_t size, uint32_t offset, uint8_t *out, unsigned length)
{
    if (offset > size || length > size - offset || offset > INT32_MAX)
        return 0;
    return !seek_file(file, offset) && read_file(file, out, length) == (int)length;
}

static int mif_resource(BrewObject *file, uint32_t size, const uint8_t *header,
                        unsigned kind, unsigned id, uint32_t *start, uint32_t *length)
{
    uint32_t index = mif_le(header + 8, 4), table = mif_le(header + 16, 4);
    unsigned count = mif_le(header + 6, 2), sections = mif_le(header + 20, 4);
    if (!count || count > 512 || !sections || sections > UINT16_MAX ||
        index < 32 || table > size || table < index || count > (table - index) / 8 ||
        sections + 1 > (size - table) / 4)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t entry[8];
        if (!mif_read(file, size, index + i * 8, entry, sizeof(entry)))
            return 0;
        if (mif_le(entry, 2) != kind || mif_le(entry + 2, 2) != id)
            continue;
        unsigned section = mif_le(entry + 6, 2);
        if (section >= sections || !mif_read(file, size, table + section * 4, entry, sizeof(entry)))
            return 0;
        uint32_t from = mif_le(entry, 4), to = mif_le(entry + 4, 4);
        if (from < table + (sections + 1) * 4 || to > size || from >= to)
            return 0;
        *start = from;
        *length = to - from;
        return 1;
    }
    return 0;
}

static void read_mif_title(BrewObject *file, uint32_t size, char *title)
{
    uint8_t header[32], bytes[TITLE_SIZE];
    uint32_t offset, length;
    if (!mif_read(file, size, 0, header, sizeof(header)) || mif_le(header, 2) != 0x11 ||
        !mif_resource(file, size, header, 0x5000, 2, &offset, &length) || length < 10 ||
        !mif_read(file, size, offset + 8, bytes, 2))
        return;
    unsigned id = mif_le(bytes, 2);
    if (!id || !mif_resource(file, size, header, 1, id, &offset, &length))
        return;
    if (length > sizeof(bytes))
        length = sizeof(bytes);
    if (length < 2 || !mif_read(file, size, offset, bytes, length))
        return;
    int wide = bytes[0] == 0xff && bytes[1] == 0xfe;
    if (!wide && bytes[0] != 2 && bytes[0] != 3)
        return;
    unsigned out = 0;
    for (unsigned i = wide ? 2 : 1; i + (unsigned)wide < length && out + 2 < TITLE_SIZE;
         i += wide ? 2 : 1) {
        unsigned c = wide ? mif_le(bytes + i, 2) : bytes[i];
        if (!c)
            break;
        if (c < 32 || (c >= 127 && c < 160))
            c = ' ';
        if (c > 255)
            c = c == 0x2019 ? '\'' : '?';
        if (c > 127) {
            title[out++] = (char)(0xc0 | (c >> 6));
            c = 0x80 | (c & 0x3f);
        }
        title[out++] = (char)c;
    }
    while (out && title[out - 1] == ' ')
        --out;
    title[out] = '\0';
}

void catalog_load_title(BrewObject *files, Catalog *catalog, unsigned index)
{
    if (catalog->title_checked[index])
        return;
    catalog->title_checked[index] = 1;
    char path[PATH_SIZE];
    BrewFileInfo entry;
    mif_path(path, catalog->games[index]);
    if (info(files, path, &entry) || (entry.attributes & ATTR_DIRECTORY))
        return;
    BrewObject *file = open_file(files, path, FILE_READ_ONLY);
    if (file)
        read_mif_title(file, entry.size, catalog->titles[index]);
    close_object(&file);
}

static unsigned title_length(const char *title)
{
    unsigned length = 0;
    for (; *title; ++title)
        length += ((uint8_t)*title & 0xc0) != 0x80;
    return length;
}

static int same_title(const char *left, const char *right)
{
    while (*left || *right) {
        while (*left == ' ' || *left == '-')
            ++left;
        while (*right == ' ' || *right == '-')
            ++right;
        if (lower((uint8_t)*left) != lower((uint8_t)*right))
            return 0;
        if (!*left)
            break;
        ++left;
        ++right;
    }
    return 1;
}

void catalog_label(const Catalog *catalog, unsigned index, char *label, unsigned limit)
{
    const char *id = catalog->games[index];
    const char *public_name = game_name(id);
    const char *installed = catalog->titles[index];
    if (!public_name) {
        text_add(label, 0, *installed ? installed : "Título indisponível", limit);
        return;
    }
    unsigned at = text_add(label, 0, public_name, limit);
    if ((!*installed || same_title(public_name, installed)) && same(id, "277380"))
        installed = "GOF";
    if (!*installed || same_title(public_name, installed) ||
        title_length(public_name) + title_length(installed) + 3 > 40)
        return;
    at = text_add(label, at, " (", limit);
    at = text_add(label, at, installed, limit);
    text_add(label, at, ")", limit);
}

static void log_line(Backup *b, const char *line)
{
    if (!b->log)
        return;
    const char *end = line;
    while (*end)
        ++end;
    if (!write_all(b->log, (const uint8_t *)line, (uint32_t)(end - line))) {
        b->log_error = file_error(b->files);
        close_object(&b->log);
    }
}

static void journal_path(Backup *b, const char *tag, const char *path);

static int missing_module_resource(const char *path)
{
    const char *name = after_prefix(path, "fs:/mod/");
    if (!name)
        return 0;
    if (same(name, "wms/imelody0.imy"))
        return 1;
    const char *extension = "";
    for (const char *p = name; *p; ++p) {
        if (*p == '/')
            return 0;
        if (*p == '.')
            extension = p;
    }
    return equal_case(extension, ".mod") || equal_case(extension, ".bar");
}

static void record(Backup *b, const char *path, int code)
{
    if (b->planning)
        return;
    char line[PATH_SIZE + 40];
    int possible_system = code == BREW_FILE_NOT_FOUND && missing_module_resource(path);
    ++b->errors;
    if (!possible_system || b->errors == 1 || b->last_error_is_system) {
        b->last_code = code;
        b->last_error_is_system = possible_system;
        text_add(b->last_error, 0, path, PATH_SIZE - 1);
    }
    unsigned at = text_add(line, 0, "ERRO ", sizeof(line) - 1);
    uint32_t number = (uint32_t)code;
    if (code < 0) {
        at = text_add(line, at, "-", sizeof(line) - 1);
        number = 0u - number;
    }
    at = text_number(line, at, number);
    at = text_add(line, at, " ", sizeof(line) - 1);
    at = text_add(line, at, path, sizeof(line) - 3);
    text_add(line, at, "\r\n", sizeof(line) - 1);
    log_line(b, line);
    if (possible_system) {
        ++b->system_missing;
        journal_path(b, "NOTA Possivel recurso de sistema ausente: ", path);
    }
}

static void log_verified_file(Backup *b)
{
    char line[PATH_SIZE + 28];
    line[0] = 'O';
    line[1] = 'K';
    line[2] = ' ';
    static const char hex[] = "0123456789abcdef";
    const uint32_t crc = b->crc ^ UINT32_MAX;
    for (unsigned i = 0; i < 8; ++i)
        line[3 + i] = hex[(crc >> (28 - i * 4)) & 15];
    line[11] = ' ';
    unsigned at = text_number(line, 12, b->expected);
    at = text_add(line, at, " ", sizeof(line) - 1);
    at = text_add(line, at, b->current, sizeof(line) - 3);
    text_add(line, at, "\r\n", sizeof(line) - 1);
    log_line(b, line);
}

static void close_copy(Backup *b)
{
    close_object(&b->source);
    close_object(&b->output);
    if (b->temporary_owned) {
        if (card_path(b->temporary))
            file_operation(b->files, FILE_REMOVE_SLOT, b->temporary);
        b->temporary_owned = 0;
    }
    b->phase = COPY_NONE;
}

static const char *state_label(BackupState state)
{
    return state == BACKUP_DONE ? "COMPLETO" :
           state == BACKUP_CANCELLED ? "CANCELADO" : "PARCIAL";
}

static void append_history(Backup *b, const char *summary)
{
    if (!b->session[0])
        return;
    BrewObject *file = open_file(b->files, HISTORY, FILE_APPEND);
    if (!file)
        file = open_file(b->files, HISTORY, FILE_CREATE);
    if (!file) {
        b->history_error = file_error(b->files);
        return;
    }
    char line[220];
    unsigned at = text_add(line, 0, "\r\n", sizeof(line) - 1);
    at = text_add(line, at, b->session + sizeof(ROOT), sizeof(line) - 1);
    at = text_add(line, at, b->include_config ? " | Backup completo" : " | Jogo/app ",
                  sizeof(line) - 1);
    if (!b->include_config)
        at = text_add(line, at, b->catalog->games[b->game_end - 1], sizeof(line) - 1);
    at = text_add(line, at, " | ", sizeof(line) - 1);
    at = text_add(line, at, state_label(b->state), sizeof(line) - 1);
    at = text_add(line, at, " | ", sizeof(line) - 1);
    at = text_add(line, at, summary, sizeof(line) - 1);
    if (!write_all(file, (const uint8_t *)line, at))
        b->history_error = file_error(b->files);
    close_object(&file);
}

static void finish(Backup *b, BackupState state)
{
    b->history_skipped = b->reading_checkpoint && !b->log;
    if (b->planning) {
        b->planning = 0;
        b->planned_bytes = 0;
    }
    close_copy(b);
    close_object(&b->checkpoint);
    b->reading_checkpoint = 0;
    while (b->depth)
        close_object(&b->folders[--b->depth].files);
    if (state == BACKUP_DONE && (b->errors || b->log_error))
        state = BACKUP_PARTIAL;
    char line[100];
    unsigned at = text_add(line, 0, "Arquivos verificados: ", sizeof(line) - 1);
    at = text_number(line, at, b->copied);
    at = text_add(line, at, " | erros: ", sizeof(line) - 1);
    at = text_number(line, at, b->errors);
    at = text_add(line, at, " | excluidos: ", sizeof(line) - 1);
    at = text_number(line, at, b->excluded);
    text_add(line, at, "\r\n", sizeof(line) - 1);
    log_line(b, line);
    char result[16];
    unsigned result_length = text_add(result, 0, state_label(state), sizeof(result) - 3);
    text_add(result, result_length, "\r\n", sizeof(result) - 1);
    log_line(b, result);
    close_object(&b->log);
    b->state = state == BACKUP_DONE && b->log_error ? BACKUP_PARTIAL : state;
    if (!b->history_skipped)
        append_history(b, line);
}

static void stop_with_error(Backup *b, const char *path, int code)
{
    if (b->planning) {
        b->planning = 0;
        b->planned_bytes = 0;
    }
    if (code == BREW_FILE_FULL || code == SD_SPACE_INSUFFICIENT) {
        b->last_operation = "Espaço insuficiente no cartão SD";
        close_copy(b);
        log_line(b, "Espaco insuficiente no cartao SD.\r\n");
    }
    record(b, path, code);
    finish(b, BACKUP_PARTIAL);
}

static int read_sd_marker(BrewObject *files)
{
    BrewObject *marker = open_file(files, ROOT "/ZB-CARD.TXT", FILE_READ_ONLY);
    if (!marker)
        return file_error(files);
    char tag[5] = {0};
    int size = read_file(marker, (uint8_t *)tag, 4);
    int code = size < 0 ? file_error(files) : size == 4 && same(tag, "ZB04") ? 0 : MARKER_ERROR;
    close_object(&marker);
    return code;
}

int sd_free_space(BrewObject *files, uint32_t *available)
{
    uint32_t total = 0, free = 0;
    int code = ((int (*)(BrewObject *, const char *, uint32_t *, uint32_t *))
        files->vt[FILE_FREE_SPACE_EX_SLOT])(files, CARD, &total, &free);
    if (code)
        return code;
    if (!total || free > total)
        return SD_SPACE_INVALID;
    *available = free;
    return 0;
}

static void diagnose_sd_failure(Backup *b)
{
    b->sd_setup_failed = 1;
    b->sd_marker_code = read_sd_marker(b->files);
    uint32_t available;
    b->sd_volume_code = sd_free_space(b->files, &available);
}

static int prepare_sd_root(Backup *b)
{
    BrewFileInfo result;
    int code = info(b->files, ROOT, &result);
    b->sd_info_code = code ? code : (result.attributes & ATTR_DIRECTORY) ? 0 : BACKUP_DESTINATION_CONFLICT;
    if (!code)
        return b->sd_info_code;

    // Ler o marcador confirma a presença de ZB quando GetInfo não reconhece diretórios no SD.
    b->sd_marker_code = read_sd_marker(b->files);
    if (!b->sd_marker_code)
        return 0;
    code = make_directory(b->files, ROOT);
    if (code)
        b->sd_last_error = last_file_error(b->files);
    return code;
}

static int create_session(Backup *b, const char **error_path)
{
    *error_path = ROOT;
    b->last_operation = "Preparar a pasta ZB no SD";
    int code = prepare_sd_root(b);
    if (code)
        return code;
    *error_path = b->session;
    b->last_operation = "Criar uma pasta de backup no SD";
    for (unsigned attempt = 1; attempt <= SESSION_LIMIT; ++attempt) {
        char id[] = "B000000";
        unsigned number = attempt;
        for (unsigned at = 6; number; --at) {
            unsigned digit = number % 10;
            id[at] = (char)('0' + digit);
            number /= 10;
        }
        path_join(b->session, ROOT, id);
        if (!file_operation(b->files, FILE_TEST_SLOT, b->session))
            continue;
        code = make_directory(b->files, b->session);
        if (!code)
            return 0;
        b->sd_last_error = last_file_error(b->files);
        if (file_operation(b->files, FILE_TEST_SLOT, b->session))
            return code;
    }
    return BACKUP_DESTINATION_CONFLICT;
}

static BrewObject *resume_checkpoint(BrewObject *files, const char *session, uint32_t *size)
{
    char path[PATH_SIZE];
    BrewFileInfo entry;
    if (!path_join(path, session, "MANIFESTO.TXT") || info(files, path, &entry) ||
        (entry.attributes & ATTR_DIRECTORY) || entry.size < sizeof(FULL_HEADER) - 1 ||
        entry.size > CHECKPOINT_LIMIT)
        return NULL;
    BrewObject *file = open_file(files, path, FILE_READ_ONLY);
    char header[sizeof(FULL_HEADER)] = {0};
    char tail[sizeof("\nCOMPLETO\r\n")] = {0};
    if (!file || !mif_read(file, entry.size, 0, (uint8_t *)header, sizeof(header) - 1) ||
        !same(header, FULL_HEADER) ||
        !mif_read(file, entry.size, entry.size - (sizeof(tail) - 1), (uint8_t *)tail, sizeof(tail) - 1) ||
        same(tail, "\nCOMPLETO\r\n") || seek_file(file, 0)) {
        close_object(&file);
        return NULL;
    }
    *size = entry.size;
    return file;
}

uint32_t backup_find_resume(BrewObject *files, char *path)
{
    char best[8] = {0};
    uint32_t resume_size = 0;
    if (enum_init(files, ROOT, 1))
        return 0;
    for (unsigned scanned = 0; scanned < SESSION_LIMIT; ++scanned) {
        BrewFileInfo entry;
        if (!enum_next(files, &entry))
            break;
        const char *name = entry_name(ROOT, entry.name, sizeof(entry.name));
        if (!name || !(entry.attributes & ATTR_DIRECTORY) || *name != 'B')
            continue;
        unsigned length = 1;
        while (name[length] >= '0' && name[length] <= '9')
            ++length;
        if (length != 7 || name[length])
            continue;
        unsigned at = 0;
        while (name[at] && name[at] == best[at])
            ++at;
        if (name[at] <= best[at])
            continue;
        char candidate[PATH_SIZE];
        uint32_t size;
        path_join(candidate, ROOT, name);
        BrewObject *file = resume_checkpoint(files, candidate, &size);
        if (!file)
            continue;
        close_object(&file);
        text_add(best, 0, name, sizeof(best) - 1);
        resume_size = size;
    }
    return best[0] && path_join(path, ROOT, best) ? resume_size : 0;
}

void backup_start(Backup *b, BrewObject *shell, BrewObject *files, const Catalog *catalog,
                  int selected_game, const char *resume_path, uint32_t *resume_index, uint32_t checkpoint_capacity)
{
    if (b->state == BACKUP_RUNNING)
        return;
    b->shell = shell;
    b->files = files;
    b->catalog = catalog;
    b->state = BACKUP_RUNNING;
    b->phase = COPY_NONE;
    b->game = selected_game < 0 ? 0 : (unsigned)selected_game;
    b->game_end = selected_game < 0 ? catalog->count : b->game + 1;
    b->include_config = selected_game < 0;
    b->planning = 0;
    b->resume_index = resume_index;
    b->reading_checkpoint = b->replacing = 0;
    b->reused = b->remaining_paths = b->zero_files = 0;
    b->planned_bytes = b->verified_bytes = 0;
    b->config = b->task = b->copied = b->excluded = b->errors = 0;
    b->system_missing = b->last_error_is_system = 0;
    b->last_code = b->log_error = b->history_error = b->history_skipped = 0;
    b->sd_setup_failed = 0;
    b->sd_info_code = b->sd_marker_code = b->sd_last_error = 0;
    b->last_operation = NULL;
    b->session[0] = b->current[0] = b->last_error[0] = '\0';

    if (b->game_end > catalog->count) {
        b->last_operation = "Ler a lista de jogos";
        stop_with_error(b, "fs:/mod/", catalog->error ? catalog->error : CATALOG_ERROR);
        return;
    }
    uint32_t available;
    if (!sd_free_space(files, &available) && !available) {
        stop_with_error(b, CARD, SD_SPACE_INSUFFICIENT);
        return;
    }
    if (resume_path) {
        b->checkpoint = resume_checkpoint(files, resume_path, &b->checkpoint_size);
        if (!b->checkpoint || b->checkpoint_size > checkpoint_capacity) {
            b->last_operation = "Sessão de retomada indisponível";
            stop_with_error(b, resume_path, CHECKPOINT_ERROR);
            return;
        }
        text_add(b->session, 0, resume_path, sizeof(b->session) - 1);
        b->checkpoint_text = (char *)(resume_index + RESUME_SLOTS);
        b->checkpoint_loaded = 0;
        b->checkpoint_offset = sizeof(FULL_HEADER) - 1;
        b->reading_checkpoint = 1;
    } else {
        const char *error_path;
        int code = create_session(b, &error_path);
        if (code) {
            diagnose_sd_failure(b);
            char path[PATH_SIZE];
            text_add(path, 0, error_path, sizeof(path) - 1);
            b->session[0] = '\0';
            stop_with_error(b, path, b->sd_last_error == BREW_FILE_FULL ? BREW_FILE_FULL : code);
            return;
        }
        b->last_operation = NULL;
    }
    char log_path[PATH_SIZE];
    path_join(log_path, b->session, "MANIFESTO.TXT");
    path_join(b->temporary, b->session, "TRANSFER.PRT");
    if (!resume_path) {
        b->log = open_file(files, log_path, FILE_CREATE);
        if (!b->log) {
            b->last_operation = "Criar o MANIFESTO.TXT no SD";
            b->sd_last_error = last_file_error(files);
            b->log_error = b->sd_last_error ? b->sd_last_error : BREW_FAILED;
            diagnose_sd_failure(b);
            stop_with_error(b, log_path, b->log_error);
            return;
        }
        log_line(b, b->include_config ? FULL_HEADER : "Zeebo Backup\r\n");
        log_line(b, "A copia e valida somente com a ultima linha COMPLETO.\r\n");
    }
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
        b->crc_table[i] = crc;
    }
    b->planning = b->include_config && !b->log_error;
}

static int destination(Backup *b, const char *source, char *out)
{
    const char *relative = after_prefix(source, "fs:/");
    if (same(source, "fs:/mcp/lctsys/flixfile.dat"))
        relative = "lctsys/flixfile.dat";
    return relative && path_join(out, b->session, relative) && card_path(out);
}

static unsigned checkpoint_line(Backup *b, uint32_t offset, char *line, unsigned capacity)
{
    unsigned bytes = b->checkpoint_size - offset;
    if (bytes >= capacity)
        bytes = capacity - 1;
    for (unsigned i = 0; i < bytes; ++i) {
        line[i] = b->checkpoint_text[offset + i];
        if (line[i] == '\n') {
            line[i && line[i - 1] == '\r' ? i - 1 : 0] = '\0';
            return i + 1;
        }
    }
    if (bytes != b->checkpoint_size - offset)
        return 0;
    line[0] = '\0'; // Última linha interrompida durante uma gravação.
    return bytes;
}

// Cada entrada guarda a posição da linha no manifesto, com a marca de visita no bit alto.
static uint32_t *checkpoint_entry(Backup *b, const char *line)
{
    unsigned hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)line; *p; ++p)
        hash = (hash ^ *p) * 16777619u;
    for (unsigned i = 0; i < RESUME_SLOTS; ++i) {
        uint32_t *slot = &b->resume_index[(hash + i) & (RESUME_SLOTS - 1)];
        if (!*slot)
            return slot;
        char previous[PATH_SIZE + 48];
        if (!checkpoint_line(b, (*slot & ~JOURNAL_SEEN) - 1, previous, sizeof(previous)))
            break;
        if (same(previous, line))
            return slot;
    }
    return NULL;
}

static int checkpoint_path(Backup *b, const char *tag, const char *path, int visit)
{
    if (!b->resume_index)
        return 0;
    char line[PATH_SIZE + 12];
    unsigned at = text_add(line, 0, tag, sizeof(line) - 1);
    text_add(line, at, path, sizeof(line) - 1);
    uint32_t *entry = checkpoint_entry(b, line);
    if (!entry)
        return CHECKPOINT_ERROR;
    if (!*entry)
        return 0;
    if (visit && !(*entry & JOURNAL_SEEN)) {
        *entry |= JOURNAL_SEEN;
        --b->remaining_paths;
    }
    return 1;
}

static void journal_path(Backup *b, const char *tag, const char *path)
{
    char line[PATH_SIZE + 48];
    unsigned at = text_add(line, 0, tag, sizeof(line) - 3);
    at = text_add(line, at, path, sizeof(line) - 3);
    text_add(line, at, "\r\n", sizeof(line) - 1);
    log_line(b, line);
}

static void record_collision(Backup *b, const char *source, const char *dest)
{
    record(b, source, BACKUP_DESTINATION_CONFLICT);
    journal_path(b, "NOTA Destino em conflito no SD: ", dest);
}

static int rename_sd(BrewObject *files, const char *source, const char *dest)
{
    if (!card_path(source) || !card_path(dest))
        return PATH_ERROR;
    return ((int (*)(BrewObject *, const char *, const char *))files->vt[FILE_RENAME_SLOT])(
        files, source, dest);
}

static int remove_sd_file(BrewObject *files, const char *path)
{
    return card_path(path) ? file_operation(files, FILE_REMOVE_SLOT, path) : PATH_ERROR;
}

static int sd_file_exists(BrewObject *files, const char *path)
{
    return !file_operation(files, FILE_TEST_SLOT, path);
}

static int recover_swap(Backup *b)
{
    char old[PATH_SIZE];
    path_join(old, b->session, "TROCA.PRT");
    if (!sd_file_exists(b->files, old))
        return 0;
    char dest[PATH_SIZE];
    if (!b->current[0] || !destination(b, b->current, dest) ||
        checkpoint_path(b, "ARQUIVO ", b->current, 0) != 1)
        return CHECKPOINT_ERROR;
    return sd_file_exists(b->files, dest) ? remove_sd_file(b->files, old) :
           rename_sd(b->files, old, dest);
}

static void reject_checkpoint(Backup *b)
{
    char path[PATH_SIZE];
    path_join(path, b->session, "MANIFESTO.TXT");
    b->last_operation = "Manifesto ilegível no SD";
    stop_with_error(b, path, CHECKPOINT_ERROR);
}

static void read_checkpoint_step(Backup *b)
{
    if (b->checkpoint) {
        unsigned size = b->checkpoint_size - b->checkpoint_loaded;
        if (size > COPY_BLOCK)
            size = COPY_BLOCK;
        int count = read_file(b->checkpoint, (uint8_t *)b->checkpoint_text + b->checkpoint_loaded, size);
        if (count <= 0 || (unsigned)count > size) {
            reject_checkpoint(b);
            return;
        }
        for (int i = 0; i < count; ++i) {
            unsigned char c = (unsigned char)b->checkpoint_text[b->checkpoint_loaded + i];
            if ((c < 32 && c != '\r' && c != '\n') || c == 127) {
                reject_checkpoint(b);
                return;
            }
        }
        b->checkpoint_loaded += (unsigned)count;
        if (b->checkpoint_loaded == b->checkpoint_size)
            close_object(&b->checkpoint);
        return;
    }
    if (b->checkpoint_offset < b->checkpoint_size) {
        char line[PATH_SIZE + 48];
        unsigned count = checkpoint_line(b, b->checkpoint_offset, line, sizeof(line));
        if (!count) {
            reject_checkpoint(b);
            return;
        }
        const char *swap = after_prefix(line, "TROCA ");
        if (swap) {
            text_add(b->current, 0, swap, sizeof(b->current) - 1);
        } else if (same(line, "TROCADO")) {
            b->current[0] = '\0';
        } else if (after_prefix(line, "DIR ") || after_prefix(line, "ARQUIVO ")) {
            // Mantém espaço no índice para que uma consulta ausente tenha terminação rápida.
            uint32_t *entry = checkpoint_entry(b, line);
            if (!entry || (!*entry && b->remaining_paths == RESUME_SLOTS * 3 / 4)) {
                stop_with_error(b, b->session, CHECKPOINT_ERROR);
                return;
            }
            if (!*entry) {
                *entry = b->checkpoint_offset + 1;
                ++b->remaining_paths;
            }
        }
        b->checkpoint_offset += count;
        return;
    }
    int code = recover_swap(b);
    if (!code && sd_file_exists(b->files, b->temporary))
        code = remove_sd_file(b->files, b->temporary);
    char path[PATH_SIZE];
    path_join(path, b->session, "MANIFESTO.TXT");
    if (!code) {
        b->log = open_file(b->files, path, FILE_APPEND);
        if (!b->log)
            code = file_error(b->files);
    }
    if (code) {
        stop_with_error(b, path, code);
        return;
    }
    log_line(b, "\n" RESUME_MARK);
    b->reading_checkpoint = 0;
    b->current[0] = '\0';
}

static int make_parents(Backup *b, char *path)
{
    char *p = path;
    for (const char *s = b->session; *s; ++s)
        ++p;
    for (++p; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            int code = ensure_directory(b->files, path);
            *p = '/';
            if (code)
                return code;
        }
    }
    return 0;
}

static void push_folder(Backup *b, const char *path)
{
    if (b->depth == DIR_DEPTH) {
        record(b, path, DEPTH_ERROR);
        return;
    }
    Folder *dir = &b->folders[b->depth];
    int code = brew_create(b->shell, FILEMGR_CLASS, &dir->files);
    if (code || !dir->files) {
        record(b, path, code ? code : BREW_FAILED);
        return;
    }
    code = enum_init(dir->files, path, 0);
    if (code) {
        close_object(&dir->files);
        record(b, path, code);
        return;
    }
    char dest[PATH_SIZE];
    if (!destination(b, path, dest)) {
        close_object(&dir->files);
        record(b, path, PATH_ERROR);
        return;
    }
    if (!b->planning) {
        int known = checkpoint_path(b, "DIR ", path, 1);
        int exists = sd_file_exists(b->files, dest);
        if (known < 0 || (exists && !known)) {
            close_object(&dir->files);
            if (known < 0)
                record(b, path, known);
            else
                record_collision(b, path, dest);
            return;
        }
        if (!exists && !known && b->include_config)
            journal_path(b, "DIR ", path);
        code = b->log_error;
        if (!code)
            code = make_parents(b, dest);
        if (!code)
            code = exists ? ensure_directory(b->files, dest) : make_directory(b->files, dest);
        if (code) {
            close_object(&dir->files);
            stop_with_error(b, dest, code);
            return;
        }
    }
    text_add(dir->path, 0, path, sizeof(dir->path) - 1);
    dir->directories = 0;
    ++b->depth;
}

static void start_stream(Backup *b)
{
    uint32_t available;
    if (!sd_free_space(b->files, &available) && b->expected > available) {
        stop_with_error(b, b->current, SD_SPACE_INSUFFICIENT);
        return;
    }
    int code = make_parents(b, b->destination);
    if (code) {
        stop_with_error(b, b->destination, code);
        return;
    }
    b->output = open_file(b->files, b->temporary, FILE_CREATE);
    if (!b->output) {
        stop_with_error(b, b->temporary, file_error(b->files));
        return;
    }
    b->temporary_owned = 1;
    b->transferred = 0;
    b->crc = b->verify_crc = UINT32_MAX;
    b->source_nonzero = 0;
    b->phase = COPY_STREAM;
}

static void start_sized_file(Backup *b, const char *path, uint32_t size)
{
    if (!destination(b, path, b->destination)) {
        record(b, path, PATH_ERROR);
        return;
    }
    if (b->planning) {
        b->planned_bytes += size;
        return;
    }
    text_add(b->current, 0, path, sizeof(b->current) - 1);
    int known = checkpoint_path(b, "ARQUIVO ", path, 1);
    int exists = sd_file_exists(b->files, b->destination);
    if (known < 0 || (!known && exists)) {
        if (known < 0)
            record(b, path, known);
        else
            record_collision(b, path, b->destination);
        return;
    }
    b->source = open_file(b->files, path, FILE_READ_ONLY);
    if (!b->source) {
        record(b, path, file_error(b->files));
        return;
    }
    b->replacing = exists;
    b->expected = size;
    b->transferred = 0;
    b->crc = b->verify_crc = UINT32_MAX;
    b->source_nonzero = 0;
    if (exists)
        b->phase = COPY_REUSE_SOURCE;
    else
        start_stream(b);
}

static void start_file(Backup *b, const char *path)
{
    BrewFileInfo source_info;
    int code = info(b->files, path, &source_info);
    if (code || (source_info.attributes & ATTR_DIRECTORY))
        record(b, path, code ? code : PATH_ERROR);
    else
        start_sized_file(b, path, source_info.size);
}

static uint32_t crc32(const uint32_t *table, uint32_t crc, const uint8_t *data, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc;
}

static void publish_verified(Backup *b)
{
    b->phase = COPY_NONE;
    ++b->copied;
    b->verified_bytes += b->expected;
    log_verified_file(b);
    if (b->expected && !b->source_nonzero) {
        ++b->zero_files;
        journal_path(b, "ZERO ", b->current);
    }
}

static void retry_source(Backup *b)
{
    close_object(&b->source);
    b->phase = COPY_NONE;
    b->source = open_file(b->files, b->current, FILE_READ_ONLY);
    if (!b->source)
        record(b, b->current, file_error(b->files));
    else
        start_stream(b);
}

static int commit_file(Backup *b)
{
    if (!b->replacing) {
        if (sd_file_exists(b->files, b->destination))
            return BACKUP_DESTINATION_CONFLICT;
        if (b->include_config)
            journal_path(b, "ARQUIVO ", b->current);
        return b->log_error ? b->log_error : rename_sd(b->files, b->temporary, b->destination);
    }
    char old[PATH_SIZE];
    path_join(old, b->session, "TROCA.PRT");
    if (sd_file_exists(b->files, old))
        return BACKUP_DESTINATION_CONFLICT;
    journal_path(b, "TROCA ", b->current);
    if (b->log_error)
        return b->log_error;
    int code = rename_sd(b->files, b->destination, old);
    if (code)
        return code;
    code = rename_sd(b->files, b->temporary, b->destination);
    int recovery = recover_swap(b);
    if (!recovery)
        log_line(b, "TROCADO\r\n");
    return code ? code : recovery;
}

static void transfer(Backup *b)
{
    unsigned remaining = b->expected - b->transferred;
    unsigned ask = remaining > COPY_BLOCK ? COPY_BLOCK : remaining ? remaining : 1;
    int count = read_file(b->source, b->buffer, ask);
    if (remaining && count > 0 && (unsigned)count <= ask) {
        b->transferred += (unsigned)count;
        if (b->phase == COPY_VERIFY || b->phase == COPY_REUSE_SD) {
            b->verify_crc = crc32(b->crc_table, b->verify_crc, b->buffer, (unsigned)count);
        } else {
            if (!b->source_nonzero) {
                for (int i = 0; i < count; ++i) {
                    if (b->buffer[i]) {
                        b->source_nonzero = 1;
                        break;
                    }
                }
            }
            b->crc = crc32(b->crc_table, b->crc, b->buffer, (unsigned)count);
            if (b->phase == COPY_STREAM && !write_all(b->output, b->buffer, (unsigned)count))
                stop_with_error(b, b->temporary, file_error(b->files));
        }
        return;
    }
    if (remaining || count != 0) {
        if (b->phase == COPY_REUSE_SD) {
            retry_source(b);
        } else if (b->phase == COPY_VERIFY) {
            stop_with_error(b, b->temporary, INTEGRITY_ERROR);
        } else {
            record(b, b->current, count < 0 ? count : INTEGRITY_ERROR);
            close_copy(b);
        }
        return;
    }
    close_object(&b->source);
    if (b->phase == COPY_REUSE_SOURCE) {
        b->source = open_file(b->files, b->destination, FILE_READ_ONLY);
        if (!b->source) {
            retry_source(b);
            return;
        }
        b->phase = COPY_REUSE_SD;
        b->transferred = 0;
        return;
    }
    if (b->phase == COPY_REUSE_SD) {
        if (b->crc == b->verify_crc) {
            ++b->reused;
            publish_verified(b);
        } else {
            retry_source(b);
        }
        return;
    }
    if (b->phase == COPY_STREAM) {
        close_object(&b->output);
        b->source = open_file(b->files, b->temporary, FILE_READ_ONLY);
        if (!b->source) {
            stop_with_error(b, b->temporary, file_error(b->files));
            return;
        }
        b->phase = COPY_VERIFY;
        b->transferred = 0;
        return;
    }
    if (b->crc != b->verify_crc) {
        stop_with_error(b, b->temporary, INTEGRITY_ERROR);
        return;
    }
    int code = commit_file(b);
    if (code) {
        stop_with_error(b, b->destination, code);
        return;
    }
    b->temporary_owned = 0;
    publish_verified(b);
}

typedef struct {
    int struct_size;
    uint8_t attributes;
    uint32_t created;
    uint32_t size;
    char *name;
    int name_capacity;
    uint16_t *description;
    int description_size;
    uint32_t *classes;
    int class_count;
} ExtendedFileInfo;

typedef char check_extended_file_info_size[sizeof(ExtendedFileInfo) == 40 ? 1 : -1];

static int next_long_entry(BrewObject *files, ExtendedFileInfo *entry)
{
    entry->name[0] = '\0';
    entry->name[entry->name_capacity - 1] = '\0';
    return ((uint8_t (*)(BrewObject *, ExtendedFileInfo *))files->vt[ENUM_NEXT_EX_SLOT])(files, entry);
}

static int excluded_path(const char *path)
{
    return same(path, "fs:/mod/26108") || same(path, "fs:/mod/32765") ||
           same(path, "fs:/mod/sys/download") || same(path, "fs:/mod/sys/priv");
}

static void walk_folder(Backup *b)
{
    Folder *dir = &b->folders[b->depth - 1];
    char raw[PATH_SIZE];
    ExtendedFileInfo entry = {sizeof(entry), 0, 0, 0, raw, sizeof(raw), NULL, 0, NULL, 0};
    if (!next_long_entry(dir->files, &entry)) {
        int code = file_error(dir->files);
        if (code == BREW_FAILED && !dir->directories) {
            dir->directories = 1;
            code = enum_init(dir->files, dir->path, 1);
            if (!code)
                return;
        } else if (code == BREW_FAILED) {
            code = 0;
        }
        if (code)
            record(b, dir->path, code);
        close_object(&dir->files);
        --b->depth;
        return;
    }

    const char *name = entry_name(dir->path, raw, sizeof(raw));
    char path[PATH_SIZE];
    if (!name || !path_join(path, dir->path, name)) {
        record(b, dir->path, PATH_ERROR);
    } else if (excluded_name(name) || excluded_path(path)) {
        if (!b->planning)
            ++b->excluded;
    } else if (entry.attributes & ATTR_DIRECTORY) {
        push_folder(b, path);
    } else {
        start_sized_file(b, path, entry.size);
    }
}

static const char *config_path(unsigned index)
{
    switch (index) {
    case 0: return "fs:/sys/hid_devices.cfg";
    case 1: return "fs:/mcp/tvenc_status.dat";
    case 2: return "fs:/mcp/lctsys/flixfile.dat";
    default: return NULL;
    }
}

void backup_step(Backup *b)
{
    if (b->state != BACKUP_RUNNING)
        return;
    if (b->reading_checkpoint) {
        read_checkpoint_step(b);
        return;
    }
    if (b->log_error) {
        stop_with_error(b, "MANIFESTO.TXT", b->log_error);
        return;
    }
    if (b->phase != COPY_NONE) {
        transfer(b);
        return;
    }
    if (b->depth) {
        walk_folder(b);
        return;
    }
    if (b->include_config) {
        if (b->task < 2) {
            push_folder(b, b->task++ == 0 ? "fs:/mod" : "fs:/mif");
            return;
        }
        const char *path = config_path(b->config++);
        if (path) {
            int code = file_operation(b->files, FILE_TEST_SLOT, path);
            if (!code) {
                start_file(b, path);
            } else {
                int detail = last_file_error(b->files);
                if (code != BREW_FILE_NOT_FOUND && code != BREW_DIR_NOT_FOUND &&
                    detail != BREW_FILE_NOT_FOUND && detail != BREW_DIR_NOT_FOUND)
                    record(b, path, detail ? detail : code);
            }
            return;
        }
    } else if (b->game < b->game_end) {
        char path[PATH_SIZE];
        const char *id = b->catalog->games[b->game];
        if (!b->task) {
            path_join(path, "fs:/mod", id);
            push_folder(b, path);
            b->task = 1;
        } else {
            mif_path(path, id);
            start_file(b, path);
            b->task = 0;
            ++b->game;
        }
        return;
    }
    if (b->planning) {
        b->planning = 0;
        b->task = b->config = 0;
        b->current[0] = '\0';
    } else {
        if (b->remaining_paths) {
            b->last_operation = "Há caminhos antigos nesta sessão";
            record(b, b->session, STALE_ERROR);
        }
        finish(b, BACKUP_DONE);
    }
}

void backup_cancel(Backup *b)
{
    if (b->state == BACKUP_RUNNING)
        finish(b, BACKUP_CANCELLED);
}
