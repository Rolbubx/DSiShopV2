// Remote : client du serveur relais (voir remote.h et remote-server/)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <curl/curl.h>
#include <nds.h>

#include "app.h"
#include "games.h"
#include "installer.h"
#include "net.h"
#include "remote.h"
#include "remote_proto.h"

#ifndef REMOTE_SERVER
#define REMOTE_SERVER "" // Défini par le Makefile (REMOTE_SERVER=...)
#endif

#define CONFIG_DIR   "/_dsishop"
#define CONFIG_FILE  "/_dsishop/remote.cfg"   // Jeton de la DSi (secret)
#define SERVER_FILE  "/_dsishop/server.txt"   // Optionnel : autre serveur

#define POLL_WAIT_S  20
#define REPLY_MAX    (16 * 1024)
#define STATUS_MAX   (48 * 1024)
#define MAX_ACKS     RREPLY_MAX_CMDS
#define EVENTS_MAX   8

static struct {
    volatile remote_state_t state;
    char msg[160];

    char root[8];
    char server[192];
    char device_id[24];
    char token[72];

    char pair_code[16];
    char pair_url[192];
    time_t pair_expires;

    char owner_name[64];
    char owner_email[96];

    volatile bool want_pair_code;
    volatile bool forget;
    bool running;
    cothread_t thread;

    CURLM *multi;
    CURL *easy;
    char *reply;
    size_t reply_len;
    char *status;

    rack_t acks[MAX_ACKS];
    int nacks;
    bool reboot_pending;

    game_t games[GAMES_MAX];
    int ngames;
    bool games_dirty;
    int last_inst_state;

    remote_event_t events[EVENTS_MAX];
    int ev_head, ev_tail;
} rm;

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

static void set_state(remote_state_t st, const char *msg)
{
    rm.state = st;
    snprintf(rm.msg, sizeof(rm.msg), "%s", msg ? msg : "");
}

static void push_event(remote_event_t ev)
{
    int next = (rm.ev_tail + 1) % EVENTS_MAX;
    if (next == rm.ev_head)
        return; // File pleine : l'événement est perdu, pas grave
    rm.events[rm.ev_tail] = ev;
    rm.ev_tail = next;
}

remote_event_t remote_poll_event(void)
{
    if (rm.ev_head == rm.ev_tail)
        return REMOTE_EV_NONE;
    remote_event_t ev = rm.events[rm.ev_head];
    rm.ev_head = (rm.ev_head + 1) % EVENTS_MAX;
    return ev;
}

static void sleep_frames(int frames)
{
    for (int i = 0; i < frames && !rm.forget && !rm.want_pair_code; i++)
        cothread_yield_irq(IRQ_VBLANK);
}

static void path_of(char *out, size_t size, const char *rel)
{
    snprintf(out, size, "%s%s", rm.root, rel);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                 s[n - 1] == '/'))
        s[--n] = '\0';
}

static void save_config(void)
{
    char dir[64], path[64];
    path_of(dir, sizeof(dir), CONFIG_DIR);
    path_of(path, sizeof(path), CONFIG_FILE);
    mkdir(dir, 0777);

    if (rm.token[0] == '\0')
    {
        remove(path);
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "server\t%s\ndevice\t%s\ntoken\t%s\n", rm.server, rm.device_id, rm.token);
    fclose(f);
}

static void load_config(void)
{
    char path[64], line[256];

    // Serveur : fichier server.txt, sinon valeur compilée
    snprintf(rm.server, sizeof(rm.server), "%s", REMOTE_SERVER);
    path_of(path, sizeof(path), SERVER_FILE);
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(line, sizeof(line), f))
        {
            trim(line);
            if (strncmp(line, "https://", 8) == 0 || strncmp(line, "http://", 7) == 0)
                snprintf(rm.server, sizeof(rm.server), "%s", line);
        }
        fclose(f);
    }
    trim(rm.server);

    // Jeton déjà obtenu (seulement s'il correspond au même serveur)
    path_of(path, sizeof(path), CONFIG_FILE);
    f = fopen(path, "r");
    if (!f)
        return;
    char server[192] = "";
    while (fgets(line, sizeof(line), f))
    {
        trim(line);
        char *val = strchr(line, '\t');
        if (!val)
            continue;
        *val++ = '\0';
        if (strcmp(line, "server") == 0)
            snprintf(server, sizeof(server), "%s", val);
        else if (strcmp(line, "device") == 0)
            snprintf(rm.device_id, sizeof(rm.device_id), "%s", val);
        else if (strcmp(line, "token") == 0)
            snprintf(rm.token, sizeof(rm.token), "%s", val);
    }
    fclose(f);

    if (strcmp(server, rm.server) != 0)
    {
        rm.device_id[0] = '\0';
        rm.token[0] = '\0';
    }
}

static bool installer_busy(void)
{
    inst_state_t s = installer_state();
    return s >= INST_RESOLVING && s <= INST_VERIFYING;
}

// ---------------------------------------------------------------------------
// HTTP non bloquant (interface "multi" de libcurl + cothreads)
// ---------------------------------------------------------------------------

static size_t on_reply(char *ptr, size_t size, size_t nmemb, void *ud)
{
    (void)ud;
    size_t n = size * nmemb;
    if (rm.reply_len + n >= REPLY_MAX)
        n = REPLY_MAX - 1 - rm.reply_len;
    memcpy(rm.reply + rm.reply_len, ptr, n);
    rm.reply_len += n;
    rm.reply[rm.reply_len] = '\0';
    return size * nmemb;
}

// Effectue une requête. Renvoie le code HTTP (0 si erreur réseau).
// Une requête "interruptible" (le long poll) est abandonnée dès qu'il faut
// réagir : nouveau QR code demandé, ou installation lancée sur la DSi.
static long request(const char *method, const char *path, const char *body,
                    long timeout_s, bool interruptible)
{
    char url[320];
    snprintf(url, sizeof(url), "%s%s", rm.server, path);

    CURL *c = rm.easy;
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DSiShop/" APP_VERSION " (Nintendo DSi)");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_reply);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    installer_setup_tls(c);

    struct curl_slist *headers = NULL;
    char auth[100];
    if (rm.token[0])
    {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", rm.token);
        headers = curl_slist_append(headers, auth);
    }
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

    if (strcmp(method, "POST") == 0)
    {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
    }

    rm.reply_len = 0;
    rm.reply[0] = '\0';

    curl_multi_add_handle(rm.multi, c);
    int running = 1;
    CURLcode result = CURLE_OK;
    while (running)
    {
        if (curl_multi_perform(rm.multi, &running) != CURLM_OK)
        {
            result = CURLE_FAILED_INIT;
            break;
        }
        if (!running)
            break;
        if (rm.forget || (interruptible && (rm.want_pair_code || installer_busy())))
        {
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        // Rend la main à l'interface : la boucle principale tourne pendant
        // qu'on attend la réponse (long polling de 20 s)
        cothread_yield_irq(IRQ_VBLANK);
    }

    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(rm.multi, &left)) != NULL)
    {
        if (m->msg == CURLMSG_DONE && m->easy_handle == c)
            result = m->data.result;
    }
    curl_multi_remove_handle(rm.multi, c);
    curl_slist_free_all(headers);

    if (result != CURLE_OK)
    {
        if (result == CURLE_PEER_FAILED_VERIFICATION)
            set_state(REMOTE_ERROR, "Certificat HTTPS refusé : vérifie la date de la console");
        else if (result == CURLE_COULDNT_RESOLVE_HOST)
            set_state(REMOTE_ERROR, "Serveur introuvable (DNS)");
        else if (result != CURLE_ABORTED_BY_CALLBACK)
            set_state(REMOTE_ERROR, "Serveur injoignable, nouvel essai…");
        return 0;
    }

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

// ---------------------------------------------------------------------------
// Étapes du protocole
// ---------------------------------------------------------------------------

static void apply_pair(const rreply_t *r)
{
    if (!r->has_pair)
        return;
    snprintf(rm.pair_code, sizeof(rm.pair_code), "%s", r->pair_code);
    snprintf(rm.pair_url, sizeof(rm.pair_url), "%s", r->pair_url);
    rm.pair_expires = time(NULL) + (r->pair_ttl > 0 ? r->pair_ttl : 600);
}

static void apply_owner(const rreply_t *r)
{
    if (!r->owner_known)
        return;

    if (r->has_owner)
    {
        // Événement seulement si le compte change (pas après une coupure réseau)
        bool changed = strcmp(rm.owner_email, r->owner_email) != 0 ||
                       strcmp(rm.owner_name, r->owner_name) != 0;
        snprintf(rm.owner_name, sizeof(rm.owner_name), "%s", r->owner_name);
        snprintf(rm.owner_email, sizeof(rm.owner_email), "%s", r->owner_email);
        set_state(REMOTE_PAIRED, "Connectée");
        if (changed)
            push_event(REMOTE_EV_PAIRED);
    }
    else
    {
        rm.owner_name[0] = rm.owner_email[0] = '\0';
        set_state(REMOTE_WAIT_PAIR, "Scanne le QR code avec ton téléphone");
        // Plus de code valide : on en redemande un
        if (rm.pair_url[0] == '\0' || time(NULL) >= rm.pair_expires)
            rm.want_pair_code = true;
    }
}

static bool do_register(void)
{
    set_state(REMOTE_CONNECTING, "Connexion au serveur…");
    rm.token[0] = '\0';
    long code = request("POST", "/api/device/register", "name\tNintendo DSi\n", 30, false);
    if (code != 200)
    {
        if (code)
            set_state(REMOTE_ERROR, code == 429 ? "Trop de tentatives, patiente un peu"
                                                : "Le serveur a refusé l'enregistrement");
        return false;
    }

    rreply_t r;
    remote_parse_reply(rm.reply, &r);
    if (!r.has_device)
    {
        set_state(REMOTE_ERROR, "Réponse du serveur invalide");
        return false;
    }
    snprintf(rm.device_id, sizeof(rm.device_id), "%s", r.device_id);
    snprintf(rm.token, sizeof(rm.token), "%s", r.device_token);
    save_config();
    apply_pair(&r);
    set_state(REMOTE_WAIT_PAIR, "Scanne le QR code avec ton téléphone");
    return true;
}

static void add_ack(const char *id, bool ok, const char *msg)
{
    if (rm.nacks >= MAX_ACKS)
        return;
    rack_t *a = &rm.acks[rm.nacks++];
    snprintf(a->id, sizeof(a->id), "%s", id);
    a->ok = ok;
    snprintf(a->message, sizeof(a->message), "%s", msg);
}

static void run_command(const rcmd_t *c)
{
    switch (c->type)
    {
        case RCMD_INSTALL:
            if (installer_busy())
                add_ack(c->id, false, "Une installation est déjà en cours");
            else if (!installer_storage_ok())
                add_ack(c->id, false, "Carte SD introuvable");
            else if (installer_start_ex(c->arg, true))
            {
                add_ack(c->id, true, "Installation lancée");
                push_event(REMOTE_EV_INSTALL_STARTED);
            }
            else
                add_ack(c->id, false, "Impossible de lancer l'installation");
            break;

        case RCMD_UNINSTALL:
            if (installer_busy())
                add_ack(c->id, false, "Attends la fin de l'installation en cours");
            else if (games_uninstall(installer_dir(), c->arg))
            {
                add_ack(c->id, true, "Désinstallé");
                rm.games_dirty = true;
                push_event(REMOTE_EV_GAMES_CHANGED);
            }
            else
                add_ack(c->id, false, "Jeu introuvable sur la carte SD");
            break;

        case RCMD_CANCEL:
            installer_cancel();
            add_ack(c->id, true, installer_busy() ? "Annulation demandée" : "Rien à annuler");
            break;

        case RCMD_REBOOT:
            add_ack(c->id, true, "Redémarrage…");
            rm.reboot_pending = true; // Après l'envoi de l'acquittement
            break;

        case RCMD_REFRESH:
            rm.games_dirty = true;
            add_ack(c->id, true, "OK");
            break;

        default:
            add_ack(c->id, false, "Commande inconnue (mets DSi Shop à jour)");
            break;
    }
}

static const char *inst_state_name(inst_state_t s)
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

static void send_status(void)
{
    inst_state_t s = installer_state();
    if ((int)s != rm.last_inst_state)
    {
        if (s == INST_DONE)
            rm.games_dirty = true;
        rm.last_inst_state = (int)s;
    }
    if (rm.games_dirty && installer_storage_ok())
    {
        rm.ngames = games_scan(installer_dir(), rm.games, GAMES_MAX);
        rm.games_dirty = false;
    }

    bool busy = installer_busy();
    rstatus_t st = {
        .state = inst_state_name(s),
        .message = (s == INST_IDLE) ? "" : installer_message(),
        .file = installer_file_name(),
        .release = installer_release(),
        .version = APP_VERSION,
        .now = busy ? installer_progress_now() : 0,
        .total = busy ? installer_progress_total() : 0,
        .free_bytes = installer_free_space(),
    };
    remote_build_status(rm.status, STATUS_MAX, &st, rm.acks, rm.nacks,
                        rm.games, rm.ngames);

    if (request("POST", "/api/device/status", rm.status, 30, false) == 200)
        rm.nacks = 0;
}

// ---------------------------------------------------------------------------
// Boucle principale du Remote
// ---------------------------------------------------------------------------

static int remote_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        if (rm.forget)
        {
            rm.forget = false;
            rm.token[0] = rm.device_id[0] = '\0';
            rm.pair_url[0] = rm.pair_code[0] = '\0';
            rm.owner_name[0] = rm.owner_email[0] = '\0';
            save_config();
        }

        if (net_state() != NET_CONNECTED)
        {
            set_state(REMOTE_WAIT_WIFI, "En attente du Wi-Fi…");
            sleep_frames(30);
            continue;
        }

        if (rm.token[0] == '\0')
        {
            if (!do_register())
                sleep_frames(5 * 60);
            continue;
        }

        if (rm.want_pair_code)
        {
            rm.want_pair_code = false;
            long code = request("POST", "/api/device/pair-code", "", 30, false);
            if (code == 401)
            {
                rm.token[0] = '\0'; // La DSi a été supprimée du serveur
                continue;
            }
            if (code == 200)
            {
                rreply_t r;
                remote_parse_reply(rm.reply, &r);
                apply_pair(&r);
                apply_owner(&r);
            }
        }

        // Pendant une installation, on répond vite pour envoyer la progression
        bool busy = installer_busy();
        char path[64];
        snprintf(path, sizeof(path), "/api/device/poll?wait=%d", busy ? 0 : POLL_WAIT_S);
        long code = request("GET", path, NULL, POLL_WAIT_S + 15, !busy);

        if (code == 401)
        {
            rm.token[0] = '\0';
            save_config();
            continue;
        }
        if (code == 0 && (rm.want_pair_code || rm.forget || (!busy && installer_busy())))
            continue; // Long poll interrompu volontairement
        if (code != 200)
        {
            if (code)
                set_state(REMOTE_ERROR, "Erreur du serveur, nouvel essai…");
            sleep_frames(5 * 60);
            continue;
        }

        rreply_t r;
        remote_parse_reply(rm.reply, &r);
        apply_owner(&r);
        for (int i = 0; i < r.ncmds; i++)
            run_command(&r.cmds[i]);

        send_status();

        if (rm.reboot_pending)
        {
            rm.reboot_pending = false;
            push_event(REMOTE_EV_REBOOT);
        }

        if (busy)
            sleep_frames(60);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------

void remote_init(void)
{
    snprintf(rm.root, sizeof(rm.root), "%s", isDSiMode() ? "sd:" : "fat:");
    load_config();
    rm.games_dirty = true;
    rm.last_inst_state = -1;
    set_state(rm.server[0] ? REMOTE_OFF : REMOTE_NO_SERVER,
              rm.server[0] ? "" : "Aucun serveur Remote configuré");
}

bool remote_has_device(void)
{
    return rm.token[0] != '\0';
}

void remote_start(void)
{
    if (rm.running || rm.server[0] == '\0')
        return;

    rm.reply = malloc(REPLY_MAX);
    rm.status = malloc(STATUS_MAX);
    rm.multi = curl_multi_init();
    rm.easy = curl_easy_init();
    if (!rm.reply || !rm.status || !rm.multi || !rm.easy)
    {
        set_state(REMOTE_ERROR, "Mémoire insuffisante");
        return;
    }

    // TLS + curl : pile confortable
    rm.thread = cothread_create(remote_thread, NULL, 96 * 1024, COTHREAD_DETACHED);
    if (rm.thread < 0)
    {
        set_state(REMOTE_ERROR, "Impossible de démarrer le Remote");
        return;
    }
    rm.running = true;
    set_state(REMOTE_CONNECTING, "Démarrage…");
}

void remote_request_pair_code(void)
{
    if (rm.state != REMOTE_PAIRED || rm.pair_url[0] == '\0' ||
        time(NULL) >= rm.pair_expires)
        rm.want_pair_code = true;
}

void remote_forget(void)
{
    rm.forget = true;
}

remote_state_t remote_state(void) { return rm.state; }
const char *remote_status_text(void) { return rm.msg; }
const char *remote_server(void) { return rm.server; }
const char *remote_pair_code(void) { return rm.pair_url[0] ? rm.pair_code : ""; }
const char *remote_owner_name(void) { return rm.owner_name; }
const char *remote_owner_email(void) { return rm.owner_email; }

const char *remote_pair_url(void)
{
    if (rm.pair_url[0] && time(NULL) >= rm.pair_expires)
    {
        rm.pair_url[0] = '\0';
        rm.want_pair_code = true;
    }
    return rm.pair_url;
}
