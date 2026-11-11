// Liaison Remote local <-> DSi (voir lremote_glue.h)

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "games.h"
#include "installer.h"
#include "lremote.h"
#include "lremote_glue.h"
#include "net.h"
#include "sha256.h"

#define EVENTS_MAX 8

static struct {
    uint8_t pool[32];
    uint64_t counter;

    char url[40];
    bool failed;

    remote_event_t events[EVENTS_MAX];
    int ev_head, ev_tail;
} g;

static game_t glue_games[GAMES_MAX];

// ---------------------------------------------------------------------------
// Aléatoire : réserve mélangée en SHA-256 (horloge, compteurs, entrées...)
// ---------------------------------------------------------------------------

void lremote_glue_entropy(const void *data, size_t len)
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, g.pool, sizeof(g.pool));
    sha256_update(&c, data, len);
    sha256_final(&c, g.pool);
}

static void random_bytes(void *buf, size_t len)
{
    uint8_t *out = buf;
    while (len > 0)
    {
        // Mélange avec l'heure et un compteur à chaque bloc
        struct { uint64_t counter; time_t t; clock_t c; } mix = { ++g.counter, time(NULL), clock() };
        lremote_glue_entropy(&mix, sizeof(mix));

        uint8_t block[32];
        sha256_ctx_t c;
        sha256_init(&c);
        sha256_update(&c, "sortie", 6);
        sha256_update(&c, g.pool, sizeof(g.pool));
        sha256_final(&c, block);

        size_t n = len < 32 ? len : 32;
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
}

// ---------------------------------------------------------------------------
// Fonctions fournies au Remote local
// ---------------------------------------------------------------------------

static void push_event(remote_event_t ev)
{
    int next = (g.ev_tail + 1) % EVENTS_MAX;
    if (next == g.ev_head)
        return;
    g.events[g.ev_tail] = ev;
    g.ev_tail = next;
}

remote_event_t lremote_glue_poll_event(void)
{
    if (g.ev_head == g.ev_tail)
        return REMOTE_EV_NONE;
    remote_event_t ev = g.events[g.ev_head];
    g.ev_head = (g.ev_head + 1) % EVENTS_MAX;
    return ev;
}

static bool busy(void)
{
    inst_state_t s = installer_state();
    return (s >= INST_RESOLVING && s <= INST_VERIFYING) || installer_checking();
}

static int list_games(game_t *out, int max)
{
    return installer_storage_ok() ? games_scan(installer_dir(), out, max) : 0;
}

static const char *state_name(inst_state_t s)
{
    switch (s)
    {
        case INST_RESOLVING:   return "resolving";
        case INST_DOWNLOADING: return "downloading";
        case INST_EXTRACTING:  return "extracting";
        case INST_VERIFYING:   return "verifying";
        case INST_DONE:        return "done";
        case INST_ERROR:       return "error";
        default:               return "idle";
    }
}

static void fill_status(rstatus_t *st)
{
    inst_state_t s = installer_state();
    bool b = s >= INST_RESOLVING && s <= INST_VERIFYING;
    st->state = state_name(s);
    st->message = s == INST_IDLE ? "" : installer_message();
    st->file = installer_file_name();
    st->release = installer_release();
    st->version = APP_VERSION;
    st->now = b ? installer_progress_now() : 0;
    st->total = b ? installer_progress_total() : 0;
    st->free_bytes = installer_free_space();
}

static const char *update_status(const char *file)
{
    switch (installer_update_status(file, NULL))
    {
        case UPD_AVAILABLE:  return "available";
        case UPD_UP_TO_DATE: return "uptodate";
        case UPD_CHECKING:   return "checking";
        default:             return "";
    }
}

static bool command(const char *type, const char *arg, char *msg, size_t size)
{
#define REPLY(ok, ...) do { snprintf(msg, size, __VA_ARGS__); return ok; } while (0)

    if (strcmp(type, "cancel") == 0)
    {
        installer_cancel();
        REPLY(true, "Annulation demandée");
    }
    if (strcmp(type, "reboot") == 0)
    {
        push_event(REMOTE_EV_REBOOT);
        REPLY(true, "Redémarrage…");
    }
    if (busy())
        REPLY(false, "Une installation ou une vérification est déjà en cours");
    if (!installer_storage_ok())
        REPLY(false, "Carte SD introuvable");

    if (strcmp(type, "install") == 0)
    {
        if (!installer_start_ex(arg, true))
            REPLY(false, "Impossible de lancer l'installation");
        push_event(REMOTE_EV_INSTALL_STARTED);
        REPLY(true, "Installation lancée");
    }
    if (strcmp(type, "update") == 0)
    {
        if (!installer_update(arg, true))
            REPLY(false, "Jeu introuvable ou installé à la main");
        push_event(REMOTE_EV_INSTALL_STARTED);
        REPLY(true, "Mise à jour lancée");
    }
    if (strcmp(type, "uninstall") == 0)
    {
        if (!games_uninstall(installer_dir(), arg))
            REPLY(false, "Jeu introuvable sur la carte SD");
        push_event(REMOTE_EV_GAMES_CHANGED);
        REPLY(true, "Désinstallé");
    }
    if (strcmp(type, "check") == 0)
    {
        int n = list_games(glue_games, GAMES_MAX);
        if (n == 0 || !installer_check_updates(glue_games, n))
            REPLY(false, "Rien à vérifier pour l'instant");
        REPLY(true, "Vérification lancée");
    }
    REPLY(false, "Commande inconnue");
#undef REPLY
}

static const lremote_backend_t backend = {
    .list_games = list_games,
    .fill_status = fill_status,
    .update_status = update_status,
    .command = command,
    .random = random_bytes,
};

// ---------------------------------------------------------------------------

void lremote_glue_init(void)
{
    struct { time_t t; clock_t c; void *p; } seed = { time(NULL), clock(), &seed };
    lremote_glue_entropy(&seed, sizeof(seed));

    // sd:/_dsishop/local.cfg (fat:/... en mode DS)
    // installer_dir() vaut "<racine>/roms/nds" : le dossier est à la racine
    char dir[256], path[256];
    const char *d = installer_dir();
    const char *root_end = strstr(d, "/roms/nds");
    int root_len = root_end ? (int)(root_end - d) : (int)strlen(d);
    int n1 = snprintf(dir, sizeof(dir), "%.*s/_dsishop", root_len, d);
    int n2 = snprintf(path, sizeof(path), "%s/local.cfg", dir);
    if (n1 < 0 || n1 >= (int)sizeof(dir) || n2 < 0 || n2 >= (int)sizeof(path))
        path[0] = '\0'; // Chemin trop long : pas de sauvegarde plutôt qu'un mauvais fichier
    else
        mkdir(dir, 0777);
    lremote_init(&backend, path);
}

const char *lremote_glue_url(void)
{
    if (net_state() != NET_CONNECTED)
        return "";
    net_info_t info;
    net_get_info(&info);
    snprintf(g.url, sizeof(g.url), "http://%s/", info.ip);
    return g.url;
}

bool lremote_glue_failed(void)
{
    return g.failed;
}

void lremote_glue_update(unsigned int frame)
{
    lremote_glue_entropy(&frame, sizeof(frame));

    bool want = lremote_enabled() && net_state() == NET_CONNECTED;
    if (want && !lremote_net_running() && !g.failed)
    {
        g.failed = !lremote_net_start(LREMOTE_PORT);
    }
    else if (!want && lremote_net_running())
    {
        lremote_net_stop();
    }
    if (!want)
        g.failed = false; // On réessaiera au prochain démarrage

    lremote_net_poll();
}
