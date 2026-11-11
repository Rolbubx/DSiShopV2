// Faux installeur pour le simulateur PC : simule les étapes sans réseau

#include <stdio.h>
#include <string.h>

#include "../../source/installer.h"

static inst_state_t state = INST_IDLE;
static int timer;
static size_t now, total;
static char msg[128] = "";

void installer_sim_tick(void);

bool installer_init(void) { return true; }
bool installer_storage_ok(void) { return true; }
const char *installer_dir(void) { return "sd:/roms/nds"; }

bool installer_start(const char *link)
{
    (void)link;
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
        total = 2400 * 1024;
        snprintf(msg, sizeof(msg), "Téléchargement de MonJeu-v1.2.zip…");
    }
    if (state == INST_DOWNLOADING)
        now = total * (size_t)(timer - 30) / 120;
    if (timer == 150)
    {
        state = INST_EXTRACTING;
        now = 0;
        snprintf(msg, sizeof(msg), "Extraction de l'archive…");
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
size_t installer_progress_now(void) { return now; }
size_t installer_progress_total(void) { return state == INST_DOWNLOADING ? total : 0; }
