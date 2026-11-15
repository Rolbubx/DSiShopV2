// Fait tourner le serveur du Remote local sur PC (mêmes fichiers C que sur la
// DSi), avec une fausse DSi derrière. Utilisé par test_lremote.py.
//
//   ./lremote_pc <port> <dossier_jeux> <fichier_config> [identifiant mot_de_passe]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../source/games.h"
#include "../../source/lremote.h"

static const char *games_dir;
static char last_cmd[400] = "";
static const char *state = "idle";

static int list_games(game_t *out, int max) { return games_scan(games_dir, out, max); }

static void fill_status(rstatus_t *st)
{
    st->state = state;
    st->message = strcmp(state, "idle") ? "Téléchargement de pong.nds…" : "";
    st->file = "pong.nds";
    st->release = "v1.1";
    st->version = "test";
    st->now = strcmp(state, "idle") ? 1024 : 0;
    st->total = strcmp(state, "idle") ? 4096 : 0;
    st->free_bytes = 3ull << 30;
}

static const char *update_status(const char *file)
{
    return strcmp(file, "Pong.nds") == 0 ? "available" : "";
}

static bool command(const char *type, const char *arg, char *msg, size_t size)
{
    snprintf(last_cmd, sizeof(last_cmd), "%s %s", type, arg);
    fprintf(stderr, "COMMANDE %s\n", last_cmd);
    if (strcmp(type, "install") == 0)
    {
        state = "downloading";
        snprintf(msg, size, "Installation lancée");
        return true;
    }
    if (strcmp(type, "uninstall") == 0)
    {
        bool ok = games_uninstall(games_dir, arg);
        snprintf(msg, size, ok ? "Désinstallé" : "Jeu introuvable");
        return ok;
    }
    if (strcmp(type, "cancel") == 0)
        state = "idle";
    snprintf(msg, size, "OK");
    return true;
}

static void random_bytes(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len)
        abort();
    fclose(f);
}

static const lremote_backend_t backend = {
    .list_games = list_games,
    .fill_status = fill_status,
    .update_status = update_status,
    .command = command,
    .random = random_bytes,
};

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage : %s port dossier config [user pass]\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    games_dir = argv[2];
    lremote_init(&backend, argv[3]);

    if (argc >= 6)
    {
        char err[128];
        if (!lremote_set_credentials(argv[4], argv[5], err, sizeof(err)))
        {
            fprintf(stderr, "identifiants refusés : %s\n", err);
            return 1;
        }
    }

    if (!lremote_net_start(atoi(argv[1])))
    {
        perror("lremote_net_start");
        return 1;
    }
    printf("PRET\n");

    while (1)
    {
        lremote_net_poll();
        usleep(5000);
    }
}
