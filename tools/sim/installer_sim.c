// Faux installeur pour le simulateur PC : simule les étapes sans réseau

#include <stdio.h>
#include <string.h>

#include "../../source/games.h"
#include "../../source/installer.h"

static inst_state_t state = INST_IDLE;
static int timer;
static uint64_t now, total;
static char msg[128] = "";
static bool remote;

// Dossier des "jeux" du simulateur (défini par sim_main.c)
char sim_games_dir[512] = "/tmp/dsishop_sim_roms";

void installer_sim_tick(void);

bool installer_init(void) { return true; }
bool installer_storage_ok(void) { return true; }
const char *installer_dir(void) { return sim_games_dir; }
void installer_setup_tls(void *curl) { (void)curl; }
uint64_t installer_free_space(void) { return 3ull << 30; }
bool installer_was_remote(void) { return remote; }

bool installer_start(const char *link)
{
    return installer_start_ex(link, false);
}

bool installer_start_ex(const char *link, bool from_remote)
{
    (void)link;
    remote = from_remote;
    state = INST_RESOLVING;
    timer = 0;
    snprintf(msg, sizeof(msg), "Recherche de la dernière release sur GitHub…");
    return true;
}

void installer_cancel(void)
{
    state = INST_ERROR;
    snprintf(msg, sizeof(msg), "Installation annulée");
}

void installer_sim_tick(void)
{
    if (state < INST_RESOLVING || state > INST_VERIFYING)
        return;
    timer++;
    if (timer == 30)
    {
        state = INST_DOWNLOADING;
        total = 6764573491ull; // ~6,3 Go : archive découpée en morceaux
        snprintf(msg, sizeof(msg), "Téléchargement de MonJeu-v1.2.zip…");
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
        snprintf(msg, sizeof(msg), "Installation terminée");
        // Crée vraiment le fichier pour que "Mes jeux" le liste
        char path[600];
        snprintf(path, sizeof(path), "%s/MonJeu.nds", sim_games_dir);
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fwrite("ROM", 1, 3, f);
            fclose(f);
        }
        games_record(sim_games_dir, "MonJeu.nds", "github.com/auteur/monjeu", "v1.2.0");
    }
}

inst_state_t installer_state(void) { return state; }
const char *installer_message(void) { return msg; }
const char *installer_file_name(void)
{
    return state >= INST_EXTRACTING ? "MonJeu.nds" : "MonJeu-v1.2.zip";
}
const char *installer_release(void) { return "v1.2.0"; }
const char *installer_dest_path(void) { return "sd:/roms/nds/MonJeu.nds"; }
inst_state_t installer_sim_state(void) { return state; }
uint64_t installer_progress_now(void) { return now; }
uint64_t installer_progress_total(void) { return state == INST_DOWNLOADING ? total : 0; }
