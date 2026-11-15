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
static void twl_sim_tick(void);

// Mise à jour de TWiLight Menu++ (simulée plus bas)
static twl_state_t twl_st = TWL_IDLE;
static int twl_timer;
static char twl_have[64] = "v27.24.0";

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
    if (twl_st == TWL_UPDATING)
    {
        twl_st = TWL_READY;   // Annulée : on pourra la reprendre
        return;
    }
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
    twl_sim_tick();
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

// ---------------------------------------------------------------------------
// Réglages : gardés en mémoire le temps de la simulation
// ---------------------------------------------------------------------------

static struct { char name[32]; char value[64]; } prefs[8];
static int nprefs;

bool installer_pref_get(const char *pref, char *out, size_t size)
{
    for (int i = 0; i < nprefs; i++)
    {
        if (strcmp(prefs[i].name, pref) == 0)
        {
            snprintf(out, size, "%s", prefs[i].value);
            return true;
        }
    }
    return false;
}

void installer_pref_set(const char *pref, const char *value)
{
    int i;
    for (i = 0; i < nprefs && strcmp(prefs[i].name, pref) != 0; i++)
        ;
    if (i == nprefs)
    {
        if (nprefs == (int)(sizeof(prefs) / sizeof(prefs[0])))
            return;
        nprefs++;
    }
    snprintf(prefs[i].name, sizeof(prefs[i].name), "%s", pref);
    snprintf(prefs[i].value, sizeof(prefs[i].value), "%s", value);
}

// ---------------------------------------------------------------------------
// Parcourir / chercher : faux résultats, disponibles tout de suite
// ---------------------------------------------------------------------------

static browse_state_t browse_st = BROWSE_IDLE;
static bool browse_search;
static char browse_who[64] = "";
static const browse_repo_t browse_fake[] = {
    { "auteur", "pong", "Pong pour Nintendo DS", 128 },
    { "auteur", "monjeu", "Un jeu de plateforme DSi", 42 },
    { "auteur", "demo-3d", "", 7 },
};

static bool browse_begin(const char *who, bool search)
{
    snprintf(browse_who, sizeof(browse_who), "%s", who);
    browse_search = search;
    browse_st = BROWSE_DONE;
    return true;
}

bool installer_browse_start(const char *user) { return browse_begin(user, false); }
bool installer_browse_search(const char *query) { return browse_begin(query, true); }
bool installer_browse_is_search(void) { return browse_search; }
browse_state_t installer_browse_state(void) { return browse_st; }
const char *installer_browse_message(void) { return ""; }
const char *installer_browse_user(void) { return browse_who; }
int installer_browse_count(void)
{
    return browse_st == BROWSE_DONE ? (int)(sizeof(browse_fake) / sizeof(browse_fake[0])) : 0;
}

const browse_repo_t *installer_browse_get(int index)
{
    if (index < 0 || index >= installer_browse_count())
        return NULL;
    return &browse_fake[index];
}

// ---------------------------------------------------------------------------
// Mise à jour de TWiLight Menu++ : simulée (9 paquets, sans réseau)
// ---------------------------------------------------------------------------

#define TWL_SIM_PACKS 9
#define TWL_SIM_TOTAL (76ull * 1024 * 1024)


bool installer_twl_present(void) { return true; }

bool installer_twl_check(void)
{
    twl_st = TWL_CHECKING;
    twl_timer = 0;
    return true;
}

bool installer_twl_update(void)
{
    if (twl_st != TWL_READY)
        return false;
    twl_st = TWL_UPDATING;
    twl_timer = 0;
    return true;
}

twl_state_t installer_twl_state(void) { return twl_st; }
const char *installer_twl_message(void) { return ""; }
const char *installer_twl_installed(void) { return twl_have; }
const char *installer_twl_latest(void) { return twl_st == TWL_IDLE || twl_st == TWL_CHECKING ? "" : "v27.25.1"; }
int installer_twl_resume(void) { return 0; }

void installer_twl_progress(int *pack, int *packs, uint64_t *done, uint64_t *all)
{
    *packs = TWL_SIM_PACKS;
    *pack = twl_timer * TWL_SIM_PACKS / 180 + 1;
    if (*pack > TWL_SIM_PACKS)
        *pack = TWL_SIM_PACKS;
    *done = TWL_SIM_TOTAL * (uint64_t)twl_timer / 180;
    *all = TWL_SIM_TOTAL;
}

static void twl_sim_tick(void)
{
    if (twl_st == TWL_CHECKING && ++twl_timer >= 20)
        twl_st = TWL_READY;
    else if (twl_st == TWL_UPDATING && ++twl_timer >= 180)
    {
        twl_st = TWL_DONE;
        snprintf(twl_have, sizeof(twl_have), "v27.25.1");
    }
}

// Lancement de DSi Shop au démarrage : gardé en mémoire
static bool autoboot_on;
void installer_set_self_path(const char *path) { (void)path; }
bool installer_autoboot_available(void) { return true; }
bool installer_autoboot_get(void) { return autoboot_on; }
bool installer_autoboot_set(bool on) { autoboot_on = on; return true; }
