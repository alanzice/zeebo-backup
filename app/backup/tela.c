#include "tela.h"

#define RGB(r, g, b) (((uint32_t)(b) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8))
typedef struct {
    uint32_t background, panel, selected, border, accent, primary, muted, alert;
} Palette;

static const Palette light = {
    RGB(233, 239, 245), RGB(250, 252, 255), RGB(205, 226, 243), RGB(142, 166, 187),
    RGB(0, 70, 126), RGB(12, 28, 44), RGB(42, 62, 81), RGB(133, 49, 18)
};
static const Palette dark = {
    RGB(16, 23, 32), RGB(25, 36, 48), RGB(35, 57, 77), RGB(88, 116, 139),
    RGB(115, 206, 255), RGB(248, 251, 255), RGB(195, 214, 230), RGB(255, 184, 126)
};

typedef struct {
    BrewObject *device;
    const Palette *palette;
} Draw;

typedef struct { int16_t x, y, width, height; } Rect;

static void fill(Draw *draw, int x, int y, int width, int height, uint32_t color)
{
    Rect rect = {(int16_t)x, (int16_t)y, (int16_t)width, (int16_t)height};
    ((void (*)(BrewObject *, const Rect *, uint32_t, uint32_t, uint32_t))draw->device->vt[DRAW_RECT_SLOT])(
        draw->device, &rect, color, color, RECT_FILL);
}

static void text(Draw *draw, int x, int y, int bold, uint32_t color, const char *value)
{
    uint16_t wide[78];
    int count = 0;
    while (count < (int)(sizeof(wide) / sizeof(wide[0])) - 1 && *value) {
        uint16_t character = (uint8_t)*value++;
        if ((character == 0xc2 || character == 0xc3) && ((uint8_t)*value & 0xc0) == 0x80)
            character = ((character & 0x1f) << 6) | ((uint8_t)*value++ & 0x3f);
        wide[count++] = character;
    }
    wide[count] = 0;
    uint32_t (*set_color)(BrewObject *, unsigned, uint32_t) =
        (uint32_t (*)(BrewObject *, unsigned, uint32_t))draw->device->vt[SET_COLOR_SLOT];
    set_color(draw->device, COLOR_USER_TEXT, color);
    ((void (*)(BrewObject *, int, const uint16_t *, int, int, int, const Rect *, uint32_t))
        draw->device->vt[DRAW_TEXT_SLOT])(draw->device, bold ? 1 : FONT_NORMAL, wide, count, x, y,
                                        NULL, TEXT_TRANSPARENT);
}


static void panel(Draw *draw, int y, int height, int selected)
{
    fill(draw, 34, y, 572, height, selected ? draw->palette->accent : draw->palette->border);
    fill(draw, selected ? 38 : 35, y + 1, selected ? 567 : 570, height - 2,
         selected ? draw->palette->selected : draw->palette->panel);
}

static void frame(Draw *draw, const char *subtitle, const char *footer)
{
    fill(draw, 0, 0, 640, 480, draw->palette->background);
    fill(draw, 34, 35, 35, 35, draw->palette->accent);
    fill(draw, 43, 44, 17, 14, draw->palette->selected);
    fill(draw, 45, 61, 12, 2, draw->palette->selected);
    text(draw, 82, 32, 1, draw->palette->primary, "Zeebo Backup v0.12");
    text(draw, 82, 58, 0, draw->palette->muted, subtitle);
    fill(draw, 34, 89, 572, 1, draw->palette->border);
    fill(draw, 34, 427, 572, 1, draw->palette->border);
    text(draw, 34, 441, 0, draw->palette->muted, footer);
}

static void summary_text(Draw *draw, int x, int y, const char *prefix, unsigned count,
                         const char *suffix, uint32_t color)
{
    char line[80];
    unsigned at = text_add(line, 0, prefix, 54);
    at = text_number(line, at, count);
    text_add(line, at, suffix, sizeof(line) - 1);
    text(draw, x, y, 0, color, line);
}

static void result_code(Draw *draw, int y, const char *label, int code)
{
    char line[76];
    unsigned at = text_add(line, 0, label, 60);
    if (code < 0)
        at = text_add(line, at, "-", 61);
    text_number(line, at, code < 0 ? 0u - (uint32_t)code : (uint32_t)code);
    text(draw, 52, y, 0, draw->palette->alert, line);
}

static void home(Draw *draw, const View *view, const Catalog *catalog)
{
    frame(draw, "Jogos e saves da memória interna para o SD", view->dark_theme ?
          "1 / START  Selecionar     R  Modo Claro     HOME ×3  Sair" :
          "1 / START  Selecionar     R  Modo Escuro     HOME ×3  Sair");
    summary_text(draw, 34, 105, "", catalog->count, " jogos e apps detectados", draw->palette->accent);
    panel(draw, 130, 58, view->home_choice == HOME_FULL);
    text(draw, 53, 135, 1, draw->palette->primary, "Backup completo");
    text(draw, 53, 163, 0, draw->palette->muted, "Árvores mod e mif, saves e configurações");
    panel(draw, 196, 58, view->home_choice == HOME_GAME);
    text(draw, 53, 201, 1, draw->palette->primary, "Escolher jogo");
    text(draw, 53, 229, 0, draw->palette->muted, "Copia um jogo, sua pasta de saves e o MIF");
    panel(draw, 262, 58, view->home_choice == HOME_HELP);
    text(draw, 53, 267, 1, draw->palette->primary, "Instruções");
    text(draw, 53, 295, 0, draw->palette->muted, "Como preparar o SD para o Memory Copy");
    panel(draw, 328, 58, view->home_choice == HOME_RESUME);
    text(draw, 53, 333, 1, draw->palette->primary, "Retomar backup");
    text(draw, 53, 361, 0, draw->palette->muted, "Confere a sessão incompleta mais recente");
    if (catalog->error)
        result_code(draw, 395, "Lista possivelmente incompleta. Código BREW: ", catalog->error);
    else if (catalog->omitted)
        text(draw, 34, 395, 0, draw->palette->alert, "A lista de jogos pode estar incompleta.");
}

static void games(Draw *draw, const View *view, const Catalog *catalog)
{
    frame(draw, "Escolher jogo", "1 Selecionar     2 / ESQUERDA  Voltar");
    if (!catalog->count) {
        text(draw, 34, 121, 1, draw->palette->primary, "Nenhuma pasta de jogo detectada");
        if (catalog->error)
            result_code(draw, 153, "Código BREW: ", catalog->error);
        return;
    }
    const unsigned end = view->first_game + GAME_ROWS;
    for (unsigned i = view->first_game; i < catalog->count && i < end; ++i) {
        int y = 110 + (int)(i - view->first_game) * 47;
        const char *id = catalog->games[i];
        const int selected = i == view->game_choice;
        char label[100];
        catalog_label(catalog, i, label, sizeof(label) - 1);
        int numbered = numeric_id(id);
        panel(draw, y, 42, selected);
        if (numbered)
            text(draw, 52, y + 11, 0, draw->palette->accent, id);
        text(draw, numbered ? 136 : 52, y + 9, 1, draw->palette->primary, label);
    }
    char line[45];
    unsigned at = text_number(line, 0, view->game_choice + 1);
    at = text_add(line, at, " de ", sizeof(line) - 1);
    text_number(line, at, catalog->count);
    text(draw, 34, 400, 0, draw->palette->muted, line);
    const char *selected_id = catalog->games[view->game_choice];
    if (!numeric_id(selected_id))
        text(draw, 146, 400, 0, draw->palette->accent, selected_id);
}

static void confirm(Draw *draw, const View *view, const Catalog *catalog)
{
    frame(draw, "Confirmar destino", "DIRECIONAL  Escolher     1  Confirmar     2  Voltar");
    panel(draw, 125, 180, 0);
    char name[100];
    if (view->selected_game < 0)
        text_add(name, 0, "Backup completo", sizeof(name) - 1);
    else
        catalog_label(catalog, (unsigned)view->selected_game, name, sizeof(name) - 1);
    text(draw, 53, 147, 1, draw->palette->primary, name);
    if (view->selected_game < 0)
        text(draw, 53, 180, 0, draw->palette->muted, "Pastas mod e mif, saves e configurações");
    else
        text(draw, 53, 180, 0, draw->palette->muted, "Pasta do jogo, saves em udata e MIF");
    text(draw, 53, 217, 0, draw->palette->accent, "Destino: cartão SD  /  ZB");
    uint32_t bytes = view->sd_available;
    if (!view->sd_space_known) {
        text(draw, 53, 253, 0, draw->palette->muted, "Espaço livre no SD indisponível");
    } else if (bytes >= 1048576) {
        summary_text(draw, 53, 253, "Espaço livre no SD: ", bytes / 1048576, " MiB", draw->palette->muted);
    } else if (bytes >= 1024) {
        summary_text(draw, 53, 253, "Espaço livre no SD: ", bytes / 1024, " KiB", draw->palette->alert);
    } else {
        summary_text(draw, 53, 253, "Espaço livre no SD: ", bytes, " bytes", draw->palette->alert);
    }
    const uint32_t back_background = view->confirm_copy ? draw->palette->panel : draw->palette->selected;
    const uint32_t copy_background = view->confirm_copy ? draw->palette->selected : draw->palette->panel;
    fill(draw, 34, 335, 181, 48, back_background);
    fill(draw, 225, 335, 381, 48, copy_background);
    if (view->confirm_copy)
        fill(draw, 225, 335, 4, 48, draw->palette->accent);
    else
        fill(draw, 34, 335, 4, 48, draw->palette->accent);
    text(draw, 53, 350, 1, draw->palette->primary, "Voltar");
    text(draw, 245, 350, 1, draw->palette->primary, "Copiar para o SD");
}

static unsigned backup_percentage(const Backup *backup)
{
    if (backup->state == BACKUP_DONE)
        return 100;
    // Busca o percentual inteiro sem precisar de divisão de 64 bits no ARM.
    const uint64_t scaled_verified = backup->verified_bytes * 100u;
    unsigned lower = 0, upper = 100;
    while (upper - lower > 1) {
        unsigned middle = (lower + upper) / 2;
        if (scaled_verified >= backup->planned_bytes * middle)
            lower = middle;
        else
            upper = middle;
    }
    return lower;
}

static void remaining_time(Draw *draw, const View *view, const Backup *backup)
{
    uint64_t worked = backup->verified_bytes;
    if (backup->phase == COPY_STREAM || backup->phase == COPY_REUSE_SOURCE)
        worked += backup->transferred / 2;
    else if (backup->phase == COPY_VERIFY || backup->phase == COPY_REUSE_SD)
        worked += ((uint64_t)backup->expected + backup->transferred) / 2;

    const unsigned elapsed = view->copy_elapsed_ms / 1000;
    if (worked >= backup->planned_bytes) {
        text(draw, 34, 304, 0, draw->palette->muted, "Finalizando...");
    } else if (elapsed < 5 || !worked) {
        text(draw, 34, 304, 0, draw->palette->muted, "Medindo velocidade...");
    } else {
        enum { MAX_ESTIMATE = 48 * 3600 };
        // Calcula o quociente arredondado para cima sem divisão de 64 bits no ARM.
        const uint64_t work_remaining = (backup->planned_bytes - worked) * elapsed;
        unsigned lower = 0, upper = MAX_ESTIMATE;
        while (upper - lower > 1) {
            unsigned middle = (lower + upper) / 2;
            if (worked * middle < work_remaining)
                lower = middle;
            else
                upper = middle;
        }
        char line[40];
        unsigned at = text_add(line, 0, "Restam ~ ", sizeof(line) - 1);
        if (upper == MAX_ESTIMATE) {
            text_add(line, at, "48 h ou mais", sizeof(line) - 1);
        } else if (upper >= 60) {
            unsigned minutes = (upper + 30) / 60;
            if (minutes >= 60) {
                at = text_number(line, at, minutes / 60);
                at = text_add(line, at, " h ", sizeof(line) - 1);
            }
            at = text_number(line, at, minutes % 60);
            text_add(line, at, " min", sizeof(line) - 1);
        } else {
            at = text_number(line, at, ((upper + 4) / 5) * 5);
            text_add(line, at, " s", sizeof(line) - 1);
        }
        text(draw, 34, 304, 0, draw->palette->muted, line);
    }
}

static void progress(Draw *draw, const View *view, const Backup *backup, int done)
{
    const char *subtitle = view->paused ? "Backup pausado" :
        backup->reading_checkpoint ? "Lendo o manifesto no SD" :
        backup->planning ? "Calculando tamanho do backup" :
        !done ? "Copiando para o SD" : backup->state == BACKUP_DONE ?
        "Backup concluído" : backup->state == BACKUP_CANCELLED ? "Backup cancelado" : "Backup parcial";
    const char *active = view->paused ? "1  Continuar     2  Cancelar" : backup->include_config ?
        "1  Pausar     2  Cancelar" : "2  Cancelar cópia";
    const char *footer = !done ? active : backup->sd_setup_failed ?
        "1 / 2  Menu     DIREITA  Detalhes do SD" : "1 / 2  Menu     HOME ×3  Sair";
    frame(draw, subtitle, footer);
    if (!done && !view->paused && (backup->planning || backup->reading_checkpoint)) {
        for (unsigned dot = 0; dot < 4; ++dot) {
            uint32_t color = dot == view->activity_frame % 4 ? draw->palette->accent : draw->palette->border;
            fill(draw, 533 + dot * 18, 66, 8, 8, color);
        }
    }
    panel(draw, 114, 176, 0);
    summary_text(draw, 53, 133, "", backup->copied, " arquivos verificados", draw->palette->accent);
    summary_text(draw, 53, 163, "Ocorrências: ", backup->errors, "", backup->errors ? draw->palette->alert : draw->palette->muted);
    if (backup->system_missing)
        summary_text(draw, 303, 163, "Possíveis recursos de sistema: ", backup->system_missing, "", draw->palette->muted);
    summary_text(draw, 53, 193, "Itens excluídos: ", backup->excluded, "", draw->palette->muted);
    if (backup->zero_files)
        summary_text(draw, 303, 193, "Só zeros: ", backup->zero_files, "", draw->palette->alert);
    int session_y = 233;
    const int show_percentage = backup->include_config && !backup->planning && backup->planned_bytes;
    const int show_eta = show_percentage && !done && !view->paused && view->clock_available;
    if (show_percentage) {
        unsigned percent = backup_percentage(backup);
        summary_text(draw, 53, 219, "", percent, backup->state == BACKUP_DONE ?
                     "% do backup" : "% dos bytes estimados verificados", draw->palette->accent);
        fill(draw, 53, 245, 534, 6, draw->palette->selected);
        if (percent)
            fill(draw, 53, 245, percent * 534 / 100, 6, draw->palette->accent);
        session_y = 263;
    }
    text(draw, 53, session_y, 0, draw->palette->primary, backup->session[0] ? backup->session : "Destino indisponível");
    if (show_eta)
        remaining_time(draw, view, backup);
    if (!done && backup->current[0] && !backup->reading_checkpoint) {
        const int file_y = show_eta ? 335 : 316;
        text(draw, 34, file_y, 1, draw->palette->primary, "Arquivo atual");
        text(draw, 34, file_y + 29, 0, draw->palette->muted, backup->current);
    } else if (done && view->timer_error) {
        result_code(draw, 318, "Erro no temporizador: ", view->timer_error);
    } else if (done && backup->errors) {
        const char *operation = backup->last_operation;
        if (!operation && backup->last_code == BACKUP_DESTINATION_CONFLICT)
            operation = "Conflito de caminho no SD";
        if (operation)
            text(draw, 52, 300, 1, draw->palette->primary, operation);
        result_code(draw, 328, backup->last_code < 0 ? "Código do app: " : "Código BREW: ", backup->last_code);
        text(draw, 52, 355, 0, draw->palette->muted, backup->last_error);
    }
    if (backup->reused)
        summary_text(draw, 34, done ? 375 : 395, "Reaproveitados do SD: ", backup->reused, "", draw->palette->muted);
    if (done && backup->session[0] && !backup->history_skipped)
        text(draw, 34, 395, 0, backup->history_error ? draw->palette->alert : draw->palette->muted, backup->history_error ?
             "Falha ao atualizar ZB/STATUS.TXT" : "Histórico em ZB/STATUS.TXT");
}

static void resume_screen(Draw *draw, const View *view)
{
    const int available = view->resume_path[0];
    frame(draw, "Retomar backup", available ? "1  Retomar     2 / ESQUERDA  Voltar" :
          "2 / ESQUERDA  Voltar");
    if (!available) {
        text(draw, 34, 135, 1, draw->palette->primary, "Nenhuma sessão elegível no SD");
        text(draw, 34, 175, 0, draw->palette->muted, "Disponível para backups completos com manifesto de retomada.");
        return;
    }
    text(draw, 34, 135, 1, draw->palette->primary, "Sessão incompleta mais recente");
    text(draw, 34, 174, 0, draw->palette->accent, view->resume_path);
    text(draw, 34, 220, 0, draw->palette->muted, "O app confere os arquivos do SD com a memória interna.");
    text(draw, 34, 250, 0, draw->palette->muted, "Só copia arquivos ausentes ou com conteúdo diferente.");
    if (view->resume_error)
        result_code(draw, 302, "Memória insuficiente no app: ", view->resume_error);
}

static void sd_details(Draw *draw, const Backup *backup)
{
    frame(draw, "Diagnóstico do cartão SD", "1 / 2 / ESQUERDA  Voltar");
    text(draw, 52, 112, 1, draw->palette->primary, backup->last_operation);
    text(draw, 52, 144, 0, draw->palette->muted, backup->last_error);
    result_code(draw, 182, "Código da operação: ", backup->last_code);
    result_code(draw, 221, "GetLastError: ", backup->sd_last_error);
    result_code(draw, 260, "GetInfo de ZB: ", backup->sd_info_code);
    result_code(draw, 299, "Leitura de ZB-CARD.TXT: ", backup->sd_marker_code);
    result_code(draw, 337, "Consulta ao volume SD: ", backup->sd_volume_code);
    const char *hint = "Leitura 0 = arquivo de teste acessível.";
    if (backup->sd_last_error == BREW_PRIVILEGE_ERROR || backup->sd_marker_code == BREW_PRIVILEGE_ERROR)
        hint = "BREW negou acesso ao SD (AEE_EPRIVLEVEL).";
    else if (backup->sd_last_error == BREW_FILE_INVALID_OPERATION ||
             backup->sd_marker_code == BREW_FILE_INVALID_OPERATION)
        hint = "BREW recusou a operação no SD (266).";
    else if (backup->sd_last_error == BREW_FILE_NO_MEDIA || backup->sd_marker_code == BREW_FILE_NO_MEDIA)
        hint = "BREW indicou ausência de mídia (267).";
    text(draw, 52, 382, 0, draw->palette->muted, hint);
}

static void instructions(Draw *draw, const View *view)
{
    frame(draw, "Memory Copy", view->dark_theme ? "2 / ESQUERDA  Voltar     R  Modo Claro" :
          "2 / ESQUERDA  Voltar     R  Modo Escuro");
    text(draw, 34, 112, 1, draw->palette->primary, "Preparar o cartão");
    text(draw, 34, 145, 0, draw->palette->muted, "Escolha uma sessão concluída em ZB.");
    text(draw, 34, 177, 0, draw->palette->muted, "Coloque as pastas mif e mod na raiz do SD.");
    text(draw, 34, 222, 1, draw->palette->primary, "No Zeebo");
    text(draw, 34, 255, 0, draw->palette->muted, "Abra EMAPPLET, Field Test, Memory Copy.");
    text(draw, 34, 287, 0, draw->palette->muted, "O Memory Copy grava do SD para o console.");
    text(draw, 34, 331, 0, draw->palette->alert, "Pode substituir arquivos que já existem.");
    text(draw, 34, 369, 0, draw->palette->muted, "Guarde uma cópia antes de restaurar.");
}

void render_backup(BrewObject *display, const View *view, const Catalog *catalog, const Backup *backup)
{
    Draw context = {display, view->dark_theme ? &dark : &light};
    Draw *draw = &context;
    switch (view->screen) {
    case HOME: home(draw, view, catalog); break;
    case GAMES: games(draw, view, catalog); break;
    case CONFIRM: confirm(draw, view, catalog); break;
    case RUNNING: progress(draw, view, backup, 0); break;
    case RESULTS: progress(draw, view, backup, 1); break;
    case SD_DETAILS: sd_details(draw, backup); break;
    case INSTRUCTIONS: instructions(draw, view); break;
    case RESUME_BACKUP: resume_screen(draw, view); break;
    case FILES_UNAVAILABLE:
        frame(draw, "Armazenamento indisponível", "HOME ×3  Sair");
        result_code(draw, 125, "IFileMgr: ", view->startup_error);
        break;
    }
    if (view->home_taps) {
        fill(draw, 34, 434, 572, 40, draw->palette->background);
        summary_text(draw, 34, 441, "Home ", view->home_taps, "/3. Repita para sair.", draw->palette->accent);
    }
    ((void (*)(BrewObject *))draw->device->vt[UPDATE_SLOT])(draw->device);
}
