// Faux installeur pour le simulateur PC : simule les étapes sans réseau, mais
// manipule de vrais fichiers (via games.c) pour la liste, les mises à jour,
// les versions précédentes et les sauvegardes.

#include <stdio.h>
#include <string.h>

#include "../../source/games.h"
#include "../../source/installer.h"

static inst_state_t state = INST_IDLE;
static int timer;
static uint64_t now, total;
static char msg[192] = "";
static bool remote, updating, up_to_date;
static char name[128] = "MonJeu-v1.2.zip";

// Dossier des "jeux" du simulateur (défini par sim_main.c)
char sim_games_dir[512] = "/tmp/dsishop_sim_roms";

// Vérification des mises à jour
static bool checking;
static int check_timer, check_total;
static struct { char file[128]; upd_status_t st; char latest[64]; } upd[GAMES_MAX];
static int nupd;
static game_t list[GAMES_MAX];

void installer_sim_tick(void);

bool installer_init(void) { return true; }
bool installer_storage_ok(void) { return true; }
const char *installer_dir(void) { return sim_games_dir; }
void installer_setup_tls(void *curl) { (void)curl; }
uint64_t installer_free_space(void) { return 3ull << 30; }
bool installer_was_remote(void) { return remote; }
bool installer_last_was_update(void) { return updating; }
bool installer_up_to_date(void) { return up_to_date; }

static void set_result(const char *file, upd_status_t st, const char *latest)
{
    int i;
    for (i = 0; i < nupd && strcmp(upd[i].file, file) != 0; i++)
        ;
    if (i == nupd)
        nupd++;
    snprintf(upd[i].file, sizeof(upd[i].file), "%s", file);
    upd[i].st = st;
    snprintf(upd[i].latest, sizeof(upd[i].latest), "%s", latest);
}

static bool start(bool from_remote)
{
    if (state >= INST_RESOLVING && state <= INST_VERIFYING)
        return false;
    remote = from_remote;
    up_to_date = false;
    state = INST_RESOLVING;
    timer = 0;
    snprintf(msg, sizeof(msg), "Recherche de la dernière release sur GitHub…");
    return true;
}

bool installer_start(const char *link)
{
    return installer_start_ex(link, false);
}

bool installer_start_ex(const char *link, bool from_remote)
{
    (void)link;
    updating = false;
    snprintf(name, sizeof(name), "MonJeu-v1.2.zip");
    return start(from_remote);
}

bool installer_update(const char *file, bool from_remote)
{
    updating = true;
    snprintf(name, sizeof(name), "%s", file);
    return start(from_remote);
}

void installer_cancel(void)
{
    state = INST_ERROR;
    snprintf(msg, sizeof(msg), "Installation annulée");
}

bool installer_check_updates(const game_t *games, int count)
{
    memcpy(list, games, sizeof(game_t) * count);
    check_total = count;
    check_timer = 0;
    checking = true;
    for (int i = 0; i < count; i++)
        set_result(games[i].file, UPD_CHECKING, "");
    return true;
}

bool installer_checking(void) { return checking; }

void installer_check_progress(int *done, int *t)
{
    *done = check_timer / 10;
    *t = check_total;
}

upd_status_t installer_update_status(const char *file, const char **latest)
{
    for (int i = 0; i < nupd; i++)
    {
        if (strcmp(upd[i].file, file) == 0)
        {
            if (latest)
                *latest = upd[i].latest;
            return upd[i].st;
        }
    }
    if (latest)
        *latest = "";
    return UPD_UNKNOWN;
}

static void write_rom(const char *file, const char *content)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", sim_games_dir, file);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fputs(content, f);
        fclose(f);
    }
}

void installer_sim_tick(void)
{
    if (checking)
    {
        check_timer++;
        if (check_timer % 10 == 0)
        {
            const game_t *g = &list[check_timer / 10 - 1];
            if (!games_is_tracked(g))
                set_result(g->file, UPD_NOT_TRACKED, "");
            else if (strcmp(g->file, "Pong.nds") == 0)
                set_result(g->file, UPD_AVAILABLE, "v1.1.0");
            else
                set_result(g->file, UPD_UP_TO_DATE, g->version);
            if (check_timer / 10 >= check_total)
                checking = false;
        }
    }

    if (state < INST_RESOLVING || state > INST_VERIFYING)
        return;
    timer++;
    if (timer == 30)
    {
        state = INST_DOWNLOADING;
        total = updating ? 160 * 1024 : 6764573491ull; // ~6,3 Go : découpé en morceaux
        snprintf(msg, sizeof(msg), "Téléchargement de %s…", updating ? "pong-v1.1.0.nds" : name);
    }
    if (state == INST_DOWNLOADING)
        now = total * (uint64_t)(timer - 30) / 120;
    if (timer == 150)
    {
        state = INST_EXTRACTING;
        now = 0;
        snprintf(msg, sizeof(msg), "Extraction de l'archive (7 morceaux de 1 Go)…");
    }
    if (timer == 180)
    {
        state = INST_VERIFYING;
        snprintf(msg, sizeof(msg), "Vérification de la ROM…");
    }
    if (timer == 190)
    {
        state = INST_DONE;
        game_t g = { 0 };
        if (updating)
        {
            // Comme le vrai : ancienne version de côté, même nom de fichier
            games_get_record(sim_games_dir, name, &g);
            games_backup(sim_games_dir, name);
            write_rom(name, "ROM v1.1.0");
            snprintf(g.version, sizeof(g.version), "v1.1.0");
            snprintf(g.url, sizeof(g.url),
                     "https://github.com/auteur/pong/releases/download/v1.1.0/pong.nds");
            set_result(name, UPD_UP_TO_DATE, "v1.1.0");
            snprintf(msg, sizeof(msg), "Mise à jour terminée (sauvegardes conservées)");
        }
        else
        {
            snprintf(name, sizeof(name), "MonJeu.nds");
            write_rom(name, "ROM");
            snprintf(g.file, sizeof(g.file), "MonJeu.nds");
            snprintf(g.source, sizeof(g.source), "github.com/auteur/monjeu");
            snprintf(g.version, sizeof(g.version), "v1.2.0");
            snprintf(g.url, sizeof(g.url),
                     "https://github.com/auteur/monjeu/releases/download/v1.2.0/MonJeu-v1.2.zip");
            snprintf(msg, sizeof(msg), "Installation terminée");
        }
        games_record(sim_games_dir, &g);
    }
}

inst_state_t installer_state(void) { return state; }
const char *installer_message(void) { return msg; }
const char *installer_file_name(void) { return name; }
const char *installer_release(void) { return updating ? "v1.1.0" : "v1.2.0"; }
const char *installer_dest_path(void) { return "sd:/roms/nds/MonJeu.nds"; }
uint64_t installer_progress_now(void) { return now; }
uint64_t installer_progress_total(void) { return state == INST_DOWNLOADING ? total : 0; }
