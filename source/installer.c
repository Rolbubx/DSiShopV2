// Installation de jeux depuis GitHub ou une URL directe

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include <curl/curl.h>
#include <fat.h>
#include <nds.h>
#include <zlib.h>

#include "games.h"
#include "installer.h"
#include "instinfo.h"
#include "splitfile.h"
#include "sysupdate.h"
#include "twlini.h"
#include "zip.h"

#define USER_AGENT "DSiShop/0.2 (Nintendo DSi; +https://github.com)"

// Taille max de la réponse JSON de l'API GitHub
// Taille max de la réponse de l'API GitHub (une seule release : quelques Ko).
// Gardée petite : en mode DS il n'y a que 4 Mo de RAM, et Mbed TLS en a besoin
// pour charger les certificats racines.
#define JSON_MAX (128 * 1024)
// Réponse du catalogue (30 lignes de texte)
#define BROWSE_JSON_MAX (32 * 1024)
// Serveur du catalogue de jeux (recherche + projets d'un utilisateur)
#define CATALOG_URL "https://dsi-shop.duckdns.org"

// Certificats racines embarqués (cacert.s)
extern const char cacert_pem[];
extern const uint32_t cacert_pem_size;

// Certificats optionnels sur la SD (remplacent ceux embarqués s'ils existent)
#define CUSTOM_CACERT "/_dsishop/cacert.pem"

static struct {
    volatile inst_state_t state;
    bool storage_ok;
    bool curl_ok;
    volatile bool cancel;
    bool remote;           // Lancée depuis le Remote (téléphone)

    char root[8];          // "sd:" ou "fat:"
    char dir[64];          // Dossier d'installation
    char link[256];        // Lien saisi par l'utilisateur

    char msg[192];
    char url[512];         // URL du fichier à télécharger
    char name[128];        // Nom du fichier
    char release[64];
    char dest[256];
    char tmp[256];         // Base du téléchargement découpé (.000, .001...)
    char tmp2[256];        // ROM extraite d'une archive

    uint64_t now, total;

    bool space_checked;
    bool no_space;
    uint64_t space_needed, space_free;

    // Pré-description, mode détaillé
    install_info_t info;
    volatile bool confirmed;   // L'utilisateur a confirmé l'installation
    bool verbose;
    time_t start_time;         // Début de l'installation (pour le journal)
    uint64_t head_size;        // Taille annoncée par une requête HEAD (0 : inconnue)
    time_t speed_t;            // Mesure du débit : dernière seconde échantillonnée
    uint64_t speed_bytes, speed;
    int next_log_pct;          // Prochain palier de progression à écrire dans le journal

    // En-têtes HTTP du dernier téléchargement (pour détecter les mises à jour)
    char etag[80];
    char modified[40];
    bool file_transfer;    // La requête en cours écrit dans un fichier

    // Mise à jour d'un jeu existant
    bool updating;
    bool up_to_date;       // Résultat : la version installée était la dernière
    game_t target;

    // Vérification des mises à jour de tous les jeux
    volatile bool checking;
    int check_done, check_total;

    cothread_t thread;
    bool thread_alive;
} inst;

// Résultats de la vérification des mises à jour, par fichier
static struct {
    char file[128];
    upd_status_t status;
    char latest[64];
} upd[GAMES_MAX];
static int nupd;
static game_t check_list[GAMES_MAX];

// Parcours des projets GitHub d'un utilisateur
static volatile browse_state_t browse_state = BROWSE_IDLE;
static char browse_user[40];   // Utilisateur, ou texte recherché
static char browse_url[320];
static bool browse_is_search;
static char browse_msg[160];
static browse_repo_t browse_list[BROWSE_MAX];
static int browse_count;

// Mise à jour de DSi Shop lui-même
static struct {
    volatile self_state_t state;
    char msg[192];
    char current[32];   // Version qui tourne
    char latest[64];    // Tag de la dernière release
    char url[512];
    bool newer;
} self;

// Mise à jour de TWiLight Menu++
static struct {
    volatile twl_state_t state;
    char msg[192];
    char installed[64];
    sysupd_manifest_t man;
    int resume;
    volatile int pack;          // Paquet en cours (index)
    sysupd_phase_t phase;
    uint64_t pack_done;         // Octets extraits du paquet en cours
    uint64_t done_before;       // Octets des paquets déjà traités
    uint64_t total;             // Téléchargement + extraction, tous paquets
} twl;

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

static void set_msg(const char *fmt, const char *arg)
{
    snprintf(inst.msg, sizeof(inst.msg), fmt, arg ? arg : "");
}

// Journal des étapes (mode détaillé) : les 16 dernières lignes
#define LOG_LINES 16
#define LOG_LEN   96
static char log_lines[LOG_LINES][LOG_LEN];
static int log_total;

static void vlog(const char *fmt, ...)
{
    if (!inst.verbose)
        return;
    char *dst = log_lines[log_total % LOG_LINES];
    int n = snprintf(dst, LOG_LEN, "[%ds] ", (int)(time(NULL) - inst.start_time));
    if (n < 0 || n >= LOG_LEN)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + n, LOG_LEN - n, fmt, ap);
    va_end(ap);
    log_total++;
}

int installer_log_count(void)
{
    return log_total < LOG_LINES ? log_total : LOG_LINES;
}

const char *installer_log_get(int index)
{
    int count = installer_log_count();
    if (index < 0 || index >= count)
        return "";
    return log_lines[(log_total - count + index) % LOG_LINES];
}

static int fail(const char *msg)
{
    vlog("Échec : %.70s", msg);
    set_msg("%s", msg);
    inst.state = INST_ERROR;
    return -1;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

// Crée un dossier et ses parents ("sd:/roms/nds")
static void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);

    // On saute le préfixe du lecteur ("sd:/")
    char *p = strchr(tmp, ':');
    p = p ? p + 2 : tmp + 1;

    for (; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// Garde un nom de fichier sûr pour FAT
static void sanitize_name(char *s)
{
    for (char *p = s; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("\\/:*?\"<>|", c))
            *p = '_';
    }
}

// Dernier élément d'un chemin/URL, sans "?query" ni "#ancre", %XX décodés
static void basename_from_url(const char *url, char *out, size_t size)
{
    const char *end = url + strcspn(url, "?#");
    const char *start = end;
    while (start > url && start[-1] != '/')
        start--;

    size_t n = 0;
    for (const char *p = start; p < end && n + 1 < size; p++)
    {
        if (*p == '%' && p + 2 < end && isxdigit((unsigned char)p[1]) &&
            isxdigit((unsigned char)p[2]))
        {
            char hex[3] = { p[1], p[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        else
        {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    sanitize_name(out);
}

// CRC16 utilisé par l'en-tête des ROM DS (init 0xFFFF, poly 0xA001)
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// Vérifie qu'un fichier est une ROM DS/DSi (CRC de l'en-tête)
static bool is_nds_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t hdr[0x160];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr))
        return false;
    uint16_t expected = hdr[0x15E] | (hdr[0x15F] << 8);
    return crc16(hdr, 0x15E) == expected;
}

// Regarde la signature "PK\3\4" au début du téléchargement
static bool is_zip(sfile_t *sf)
{
    uint8_t sig[4] = { 0 };
    return sfile_seek(sf, 0) && sfile_read(sf, sig, 4) == 4 &&
           sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
}

// "1,4 Go", "820 Mo"...
static void format_size(char *out, size_t size, uint64_t bytes)
{
    const uint64_t MB = 1024 * 1024, GB = MB * 1024;
    if (bytes >= GB)
        snprintf(out, size, "%u,%u Go", (unsigned)(bytes / GB),
                 (unsigned)((bytes % GB) * 10 / GB));
    else
        snprintf(out, size, "%u Mo", (unsigned)((bytes + MB - 1) / MB));
}

// Espace libre sur la carte SD
static bool free_space(uint64_t *out)
{
    struct statvfs st;
    if (statvfs(inst.dir, &st) != 0)
        return false;
    uint64_t block = st.f_frsize ? st.f_frsize : st.f_bsize;
    *out = (uint64_t)st.f_bavail * block;
    return true;
}

// ---------------------------------------------------------------------------
// HTTP (libcurl)
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len, cap;
} membuf_t;

static size_t write_mem(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    membuf_t *m = userdata;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->cap)
        return 0; // Trop gros : curl renvoie CURLE_WRITE_ERROR
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

// Écrit dans un fichier découpé : un nouveau morceau tous les 1 Go, ce qui
// permet de dépasser la limite de 4 Go par fichier du FAT32
static size_t write_file(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return sfile_write((sfile_t *)userdata, ptr, size * nmemb);
}

// Récupère ETag et Last-Modified (de la dernière réponse, après redirections)
static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud)
{
    (void)ud;
    size_t len = size * nitems;
    char line[256];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, buf, n);
    line[n] = '\0';
    line[strcspn(line, "\r\n")] = '\0';

    if (strncmp(line, "HTTP/", 5) == 0)
    {
        // Nouvelle réponse (redirection) : on oublie les en-têtes précédents
        inst.etag[0] = '\0';
        inst.modified[0] = '\0';
    }
    else if (strncasecmp(line, "etag:", 5) == 0)
    {
        const char *v = line + 5;
        while (*v == ' ')
            v++;
        // Tronqué si énorme : sans danger, on compare toujours la même valeur
        snprintf(inst.etag, sizeof(inst.etag), "%.79s", v);
    }
    else if (strncasecmp(line, "last-modified:", 14) == 0)
    {
        const char *v = line + 14;
        while (*v == ' ')
            v++;
        snprintf(inst.modified, sizeof(inst.modified), "%.39s", v);
    }
    return len;
}

static int progress_cb(void *p, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)p; (void)ultotal; (void)ulnow;
    inst.now = (uint64_t)dlnow;
    inst.total = (uint64_t)dltotal;
    if (inst.cancel)
        return 1;

    if (inst.file_transfer)
    {
        // Débit : octets reçus pendant la dernière seconde écoulée
        time_t t = time(NULL);
        if (t != inst.speed_t)
        {
            if (inst.speed_t != 0 && t > inst.speed_t)
                inst.speed = ((uint64_t)dlnow - inst.speed_bytes) / (uint64_t)(t - inst.speed_t);
            inst.speed_t = t;
            inst.speed_bytes = (uint64_t)dlnow;
        }
        // Journal : un point tous les 25 %
        if (inst.verbose && dltotal > 0 && (int)((uint64_t)dlnow * 100 / (uint64_t)dltotal) >= inst.next_log_pct)
        {
            char a[24], b[24];
            format_size(a, sizeof(a), (uint64_t)dlnow);
            format_size(b, sizeof(b), (uint64_t)dltotal);
            vlog("Téléchargé : %d %% (%s sur %s)", inst.next_log_pct, a, b);
            inst.next_log_pct += 25;
        }
    }

    // Dès que la taille est connue, on vérifie qu'il y a la place sur la SD
    if (inst.file_transfer && !inst.space_checked && dltotal > 0)
    {
        inst.space_checked = true;
        uint64_t avail;
        if (free_space(&avail) && (uint64_t)dltotal > avail)
        {
            inst.no_space = true;
            inst.space_needed = (uint64_t)dltotal;
            inst.space_free = avail;
            return 1;
        }
    }
    cothread_yield(); // Laisse l'interface se mettre à jour
    return 0;
}

// Vérification des certificats HTTPS (on installe des exécutables !).
// Aussi utilisé par le Remote (remote.c).
void installer_setup_tls(void *handle)
{
    CURL *curl = handle;
    // curl garde des pointeurs vers ces données : elles doivent rester valides
    static char custom[64];
    static struct curl_blob blob;

    snprintf(custom, sizeof(custom), "%s%s", inst.root, CUSTOM_CACERT);
    blob.data = (void *)cacert_pem;
    // Mbed TLS lit un octet de plus que la longueur donnée (le zéro final) :
    // on ne compte donc pas l'octet nul de cacert.s dans la taille
    blob.len = cacert_pem_size - 1;
    blob.flags = CURL_BLOB_NOCOPY;

    if (file_exists(custom))
        curl_easy_setopt(curl, CURLOPT_CAINFO, custom);
    else
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

uint64_t installer_free_space(void)
{
    uint64_t avail = 0;
    if (!inst.storage_ok || !free_space(&avail))
        return 0;
    return avail;
}

// Effectue une requête. Écrit dans un fichier (file != NULL), en mémoire
// (mem != NULL), ou rien (requête HEAD). En cas d'erreur, renvoie -1 et met
// un message lisible dans err (sans toucher à l'état de l'installation).
static int http_req(const char *url, bool github_api, sfile_t *file, membuf_t *mem,
                    char *err, size_t err_size)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        snprintf(err, err_size, "Erreur interne : curl_easy_init()");
        return -1;
    }

    struct curl_slist *headers = NULL;
    if (github_api)
    {
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    }

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Les releases redirigent
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);

    installer_setup_tls(curl);

    inst.etag[0] = '\0';
    inst.modified[0] = '\0';
    inst.head_size = 0;
    inst.speed_t = 0;
    inst.speed_bytes = inst.speed = 0;
    inst.next_log_pct = 25;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    inst.file_transfer = file != NULL;

    if (!file && !mem)
    {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD
    }
    else if (file)
    {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, mem);
    }

    inst.now = 0;
    inst.total = 0;
    inst.space_checked = false;
    inst.no_space = false;

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (!file && !mem && res == CURLE_OK)
    {
        // Requête HEAD : taille annoncée par le serveur
        curl_off_t cl = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        inst.head_size = cl > 0 ? (uint64_t)cl : 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK)
        return 0;

#define ERR(...) do { snprintf(err, err_size, __VA_ARGS__); return -1; } while (0)
    switch (res)
    {
        case CURLE_ABORTED_BY_CALLBACK:
            if (inst.no_space)
            {
                char need[24], have[24];
                format_size(need, sizeof(need), inst.space_needed);
                format_size(have, sizeof(have), inst.space_free);
                ERR("Pas assez de place sur la carte SD : il faut %s, il reste %s",
                    need, have);
            }
            ERR("Opération annulée");
        case CURLE_COULDNT_RESOLVE_HOST:
            ERR("Serveur introuvable (DNS). Vérifie le lien et la connexion.");
        case CURLE_COULDNT_CONNECT:
            ERR("Impossible de joindre le serveur");
        case CURLE_OPERATION_TIMEDOUT:
            ERR("Le serveur ne répond plus (délai dépassé)");
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
            ERR("Certificat HTTPS refusé. Vérifie la date et l'heure de la "
                "console dans les paramètres système.");
        case CURLE_WRITE_ERROR:
            ERR("%s", file ? "Impossible d'écrire sur la carte SD (pleine ?)"
                           : "Réponse du serveur trop volumineuse");
        case CURLE_HTTP_RETURNED_ERROR:
            if (code == 404)
                ERR("%s", github_api ? "Projet introuvable, ou aucune release publiée"
                                     : "Fichier introuvable (erreur 404)");
            if (code == 403 && github_api)
                ERR("GitHub limite les requêtes : réessaie dans une heure");
            ERR("Le serveur a répondu avec l'erreur HTTP %ld", code);
        default:
            ERR("Erreur réseau (%d) : %s%s%.100s", (int)res, curl_easy_strerror(res),
                errbuf[0] ? " / " : "", errbuf);
    }
#undef ERR
}

// Version pour l'installation : une erreur fait échouer l'installation
static int http_get(const char *url, bool github_api, sfile_t *file, membuf_t *mem)
{
    char err[192];
    if (http_req(url, github_api, file, mem, err, sizeof(err)) == 0)
        return 0;
    if (inst.cancel && !inst.no_space)
        return fail("Installation annulée");
    return fail(err);
}

// ---------------------------------------------------------------------------
// GitHub
// ---------------------------------------------------------------------------

// Reconnaît "github.com/auteur/projet[/releases/tag/xxx]" ou "auteur/projet".
// Renvoie false si ce n'est pas un projet GitHub (ou un lien vers un fichier).
static bool parse_github(const char *link, char *api_url, size_t size)
{
    const char *p = link;
    if (starts_with_ci(p, "https://"))
        p += 8;
    else if (starts_with_ci(p, "http://"))
        p += 7;
    if (starts_with_ci(p, "www."))
        p += 4;

    bool explicit_host = false;
    if (starts_with_ci(p, "github.com/"))
    {
        p += 11;
        explicit_host = true;
    }
    else
    {
        // Forme courte "auteur/projet" : pas de point avant le premier '/'
        const char *slash = strchr(p, '/');
        if (!slash || memchr(p, '.', slash - p))
            return false;
    }

    char owner[64], repo[96];
    size_t n = strcspn(p, "/");
    if (n == 0 || n >= sizeof(owner))
        return false;
    memcpy(owner, p, n);
    owner[n] = '\0';
    p += n;
    if (*p != '/')
        return false;
    p++;

    n = strcspn(p, "/?#");
    if (n == 0 || n >= sizeof(repo))
        return false;
    memcpy(repo, p, n);
    repo[n] = '\0';
    if (ends_with_ci(repo, ".git"))
        repo[strlen(repo) - 4] = '\0';
    p += n;

    // Lien direct vers un fichier : on le télécharge tel quel
    if (strstr(p, "/releases/download/") || strstr(p, "/raw/") ||
        strstr(p, "/blob/"))
        return false;

    const char *tag = strstr(p, "/releases/tag/");
    if (tag)
    {
        char tagname[64];
        tag += strlen("/releases/tag/");
        n = strcspn(tag, "/?#");
        if (n == 0 || n >= sizeof(tagname))
            return false;
        memcpy(tagname, tag, n);
        tagname[n] = '\0';
        snprintf(api_url, size, CATALOG_URL "/api/repos/%s/%s/releases/tags/%s",
                 owner, repo, tagname);
    }
    else
    {
        // Forme courte sans "github.com" : seulement "auteur/projet" exact
        if (!explicit_host && *p != '\0' && *p != '/')
            return false;
        snprintf(api_url, size, CATALOG_URL "/api/repos/%s/%s/releases/latest",
                 owner, repo);
    }
    return true;
}

// Lit la chaîne JSON qui suit la clé à partir de pos. Renvoie la fin ou NULL.
static const char *json_string_after(const char *pos, const char *key,
                                     char *out, size_t size)
{
    const char *k = strstr(pos, key);
    if (!k)
        return NULL;
    const char *p = k + strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    if (*p != '"')
        return NULL;
    p++;

    size_t n = 0;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\' && *p)
        {
            char e = *p++;
            switch (e)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'u': c = '?'; p += (strlen(p) >= 4) ? 4 : strlen(p); break;
                default:  c = e; break; // \" \\ \/
            }
        }
        if (n + 1 < size)
            out[n++] = c;
    }
    out[n] = '\0';
    return *p ? p + 1 : p;
}

// Lien vers un fichier de release ("github.com/a/b/releases/download/v1/x.nds") :
// renvoie l'API de la dernière release du même projet, pour les mises à jour
static bool github_repo_of_download(const char *url, char *api_url, size_t size)
{
    const char *p = strstr(url, "github.com/");
    if (!p)
        return false;
    p += 11;
    const char *dl = strstr(p, "/releases/download/");
    if (!dl)
        return false;
    // p..dl = "auteur/projet"
    const char *slash = memchr(p, '/', dl - p);
    if (!slash || slash == p || slash + 1 >= dl || (dl - p) > 150)
        return false;
    snprintf(api_url, size, CATALOG_URL "/api/repos/%.*s/releases/latest",
             (int)(dl - p), p);
    return true;
}

// Choisit le meilleur fichier de la release : extension préférée d'abord
// (celle de la version installée, lors d'une mise à jour), puis .nds > .dsi > .zip
// Taille (0 : inconnue) du fichier choisi par le dernier pick_asset()
static uint64_t last_asset_size;

static bool pick_asset(const char *json, const char *prefer_ext, char *tag,
                       size_t tag_size, char *url_out, size_t url_size,
                       char *name_out, size_t name_size)
{
    const char *exts[4] = { prefer_ext, ".nds", ".dsi", ".zip" };
    char url[512];

    json_string_after(json, "\"tag_name\"", tag, tag_size);

    for (size_t e = 0; e < 4; e++)
    {
        if (!exts[e] || !exts[e][0])
            continue;
        const char *pos = json;
        while ((pos = json_string_after(pos, "\"browser_download_url\"", url,
                                        sizeof(url))) != NULL)
        {
            char name[128];
            basename_from_url(url, name, sizeof(name));
            if (ends_with_ci(name, exts[e]))
            {
                last_asset_size = instinfo_asset_size(json, pos);
                snprintf(url_out, url_size, "%s", url);
                snprintf(name_out, name_size, "%s", name);
                return true;
            }
        }
    }
    return false;
}

// Interroge l'API GitHub : tag + fichier à télécharger. -1 et err si échec.
static int github_latest(const char *api_url, const char *prefer_ext, char *tag,
                         size_t tag_size, char *url, size_t url_size, char *name,
                         size_t name_size, char *err, size_t err_size)
{
    membuf_t mem = { .data = malloc(JSON_MAX), .len = 0, .cap = JSON_MAX };
    if (!mem.data)
    {
        snprintf(err, err_size, "Mémoire insuffisante");
        return -1;
    }
    mem.data[0] = '\0';

    int ret = http_req(api_url, true, NULL, &mem, err, err_size);
    if (ret == 0 && !pick_asset(mem.data, prefer_ext, tag, tag_size, url, url_size,
                                name, name_size))
    {
        snprintf(err, err_size, "La release ne contient aucun fichier .nds, .dsi ou .zip");
        ret = -1;
    }
    free(mem.data);
    return ret;
}

static int resolve_github(const char *api_url, const char *prefer_ext)
{
    inst.state = INST_RESOLVING;
    char owner[40] = "", repo[64] = "";
    instinfo_repo_of_api_url(api_url, owner, sizeof(owner), repo, sizeof(repo));
    if (inst.verbose && owner[0])
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Recherche de la dernière release de %s/%s sur GitHub…",
                 owner, repo);
        set_msg("%s", msg);
        vlog("Projet GitHub : %s/%s", owner, repo);
    }
    else
        set_msg("Recherche de la dernière release sur GitHub…", NULL);

    char err[192];
    if (github_latest(api_url, prefer_ext, inst.release, sizeof(inst.release),
                      inst.url, sizeof(inst.url), inst.name, sizeof(inst.name),
                      err, sizeof(err)) != 0)
        return fail(inst.cancel ? "Installation annulée" : err);

    char size[24] = "taille inconnue";
    if (last_asset_size)
        format_size(size, sizeof(size), last_asset_size);
    vlog("Release %.30s : %.40s (%s)", inst.release, inst.name, size);
    return 0;
}

// Extension (".zip"...) du fichier pointé par une URL
static void url_ext(const char *url, char *out, size_t size)
{
    char name[128];
    basename_from_url(url, name, sizeof(name));
    const char *dot = strrchr(name, '.');
    snprintf(out, size, "%s", dot ? dot : "");
}

// Compare deux fichiers octet par octet
static bool same_content(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool same = fa && fb;
    static uint8_t ba[8192], bb[8192];
    while (same)
    {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0)
            same = false;
        if (na == 0)
            break;
        cothread_yield();
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return same;
}

static void set_update_result(const char *file, upd_status_t st, const char *latest)
{
    int i;
    for (i = 0; i < nupd; i++)
        if (strcmp(upd[i].file, file) == 0)
            break;
    if (i == nupd)
    {
        if (nupd >= GAMES_MAX)
            return;
        nupd++;
    }
    snprintf(upd[i].file, sizeof(upd[i].file), "%s", file);
    upd[i].status = st;
    snprintf(upd[i].latest, sizeof(upd[i].latest), "%s", latest ? latest : "");
}

// ---------------------------------------------------------------------------
// Extraction ZIP (zip.c : ZIP et ZIP64, lu à travers les morceaux)
// ---------------------------------------------------------------------------

static int on_zip_progress(void *ud, uint64_t done, uint64_t total)
{
    (void)ud;
    inst.now = done;
    inst.total = total;
    cothread_yield();
    return inst.cancel ? 1 : 0;
}

static int extract_zip(sfile_t *sf, const char *out_path)
{
    inst.state = INST_EXTRACTING;
    inst.now = 0;
    inst.total = 0;
    unsigned int parts = sfile_part_count(sf);
    if (parts > 1)
        snprintf(inst.msg, sizeof(inst.msg),
                 "Extraction de l'archive (%u morceaux de 1 Go)…", parts);
    else if (inst.verbose)
        set_msg("Extraction de la ROM depuis l'archive .zip…", NULL);
    else
        set_msg("Extraction de l'archive…", NULL);
    vlog("Archive .zip : recherche de la ROM (.nds / .dsi)");

    char name[128] = "";
    zip_err_t err = zip_extract_rom(sf, out_path, name, sizeof(name),
                                    on_zip_progress, NULL);
    if (err != ZIP_OK)
        return fail(zip_strerror(err));

    snprintf(inst.name, sizeof(inst.name), "%s", name);
    vlog("ROM extraite : %.60s", name);
    return 0;
}

// ---------------------------------------------------------------------------
// Tâche d'installation (cothread)
// ---------------------------------------------------------------------------

// Pré-description : rassemble ce qu'on sait de l'installation (nom, auteur,
// version, taille...) puis attend que l'utilisateur confirme sur la console
static int preview_and_wait(bool github, const char *api_url)
{
    install_info_t *i = &inst.info;
    memset(i, 0, sizeof(*i));
    snprintf(i->name, sizeof(i->name), "%s", inst.name);
    i->archive = ends_with_ci(inst.name, ".zip");
    i->github = github;

    if (github)
    {
        if (instinfo_repo_of_api_url(api_url, i->author, sizeof(i->author), i->project,
                                     sizeof(i->project)))
            snprintf(i->source, sizeof(i->source), "github.com/%s/%s", i->author, i->project);
        snprintf(i->version, sizeof(i->version), "%s", inst.release);
        i->size = last_asset_size;
    }
    else
    {
        // Lien direct : le site nous donne la taille (requête HEAD), s'il veut bien
        instinfo_url_host(inst.url, i->source, sizeof(i->source));
        set_msg("Lecture des informations du fichier…", NULL);
        vlog("Lien direct : demande de la taille à %s", i->source);
        char err[192];
        if (http_req(inst.url, false, NULL, NULL, err, sizeof(err)) != 0 && inst.cancel)
            return fail("Installation annulée");
        i->size = inst.head_size;
        if (i->size == 0)
            vlog("Le site n'indique pas la taille du fichier");
    }

    inst.confirmed = false;
    inst.state = INST_PREVIEW;
    set_msg("Vérifie les informations, puis confirme", NULL);
    vlog("En attente de ta confirmation");
    while (!inst.confirmed && !inst.cancel)
        cothread_yield();
    if (inst.cancel)
        return fail("Installation annulée");
    vlog("Confirmé");
    return 0;
}

static int do_install(void)
{
    inst.release[0] = '\0';
    inst.name[0] = '\0';
    inst.dest[0] = '\0';
    inst.up_to_date = false;
    last_asset_size = 0;
    memset(&inst.info, 0, sizeof(inst.info));

    mkdir_p(inst.dir);
    snprintf(inst.tmp, sizeof(inst.tmp), "%s/.dsishop_download", inst.dir);
    snprintf(inst.tmp2, sizeof(inst.tmp2), "%s/.dsishop_extract.tmp", inst.dir);

    // 1. Lien GitHub -> URL du fichier de la dernière release
    //    (pour une mise à jour, un lien vers un fichier de release GitHub
    //    renvoie aussi vers la dernière release du projet)
    char api_url[320];
    char prefer_ext[16] = "";
    if (inst.updating)
        url_ext(inst.target.url[0] ? inst.target.url : inst.link, prefer_ext,
                sizeof(prefer_ext));

    bool github = parse_github(inst.link, api_url, sizeof(api_url)) ||
                  (inst.updating &&
                   (github_repo_of_download(inst.link, api_url, sizeof(api_url)) ||
                    github_repo_of_download(inst.target.url, api_url, sizeof(api_url))));

    if (github)
    {
        if (resolve_github(api_url, prefer_ext) != 0)
            return -1;

        // Même release que celle installée : rien à faire
        if (inst.updating && inst.target.version[0] &&
            strcmp(inst.release, inst.target.version) == 0)
        {
            snprintf(inst.name, sizeof(inst.name), "%s", inst.target.file);
            inst.up_to_date = true;
            set_update_result(inst.target.file, UPD_UP_TO_DATE, inst.release);
            set_msg("%s est déjà à jour", inst.target.file);
            inst.state = INST_DONE;
            return 0;
        }
    }
    else if (inst.updating)
    {
        // Lien direct : on demande juste les en-têtes (HEAD) au serveur
        snprintf(inst.url, sizeof(inst.url), "%s",
                 inst.target.url[0] ? inst.target.url : inst.link);
        basename_from_url(inst.url, inst.name, sizeof(inst.name));
        inst.state = INST_RESOLVING;
        set_msg("Recherche d'une nouvelle version…", NULL);

        char err[192];
        if (http_req(inst.url, false, NULL, NULL, err, sizeof(err)) != 0)
            return fail(inst.cancel ? "Mise à jour annulée" : err);

        bool same = (inst.target.etag[0] && strcmp(inst.etag, inst.target.etag) == 0) ||
                    (!inst.etag[0] && inst.target.modified[0] &&
                     strcmp(inst.modified, inst.target.modified) == 0);
        if (same)
        {
            snprintf(inst.name, sizeof(inst.name), "%s", inst.target.file);
            inst.up_to_date = true;
            set_update_result(inst.target.file, UPD_UP_TO_DATE, "");
            set_msg("%s est déjà à jour", inst.target.file);
            inst.state = INST_DONE;
            return 0;
        }
        // Sinon (fichier changé, ou serveur sans ETag ni date) : on télécharge,
        // et on comparera le contenu à la fin
    }
    else
    {
        if (strstr(inst.link, "://"))
            snprintf(inst.url, sizeof(inst.url), "%s", inst.link);
        else
            snprintf(inst.url, sizeof(inst.url), "https://%s", inst.link);
        basename_from_url(inst.url, inst.name, sizeof(inst.name));
        if (inst.name[0] == '\0')
            snprintf(inst.name, sizeof(inst.name), "jeu.nds");
    }

    // 1b. Pré-description (installation lancée depuis la console seulement : une
    //     demande du Remote ou une mise à jour n'a personne pour confirmer)
    if (!inst.updating && !inst.remote && preview_and_wait(github, api_url) != 0)
        return -1;

    // 2. Téléchargement
    inst.state = INST_DOWNLOADING;
    set_msg("Téléchargement de %s…", inst.name);
    {
        char host[48];
        instinfo_url_host(inst.url, host, sizeof(host));
        vlog("Téléchargement depuis %s", host);
    }
    time_t download_start = time(NULL);

    // Le fichier est écrit en morceaux de 1 Go (.000, .001...) : un
    // téléchargement de plus de 4 Go passe donc sur une SD en FAT32.
    sfile_t sf;
    if (!sfile_open_write(&sf, inst.tmp, SPLIT_CHUNK_DEFAULT))
        return fail("Impossible d'écrire sur la carte SD");
    int ret = http_get(inst.url, false, &sf, NULL);
    bool write_ok = sfile_close(&sf);
    if (ret != 0)
        return -1;
    if (!write_ok)
        return fail("Erreur d'écriture sur la carte SD (pleine ?)");

    {
        char size[24], speed[24];
        long secs = (long)(time(NULL) - download_start);
        format_size(size, sizeof(size), inst.now);
        instinfo_format_speed(speed, sizeof(speed), secs > 0 ? inst.now / (uint64_t)secs : 0);
        vlog("Téléchargement terminé : %s en %ld s%s%s", size, secs, speed[0] ? ", " : "", speed);
    }

    if (!sfile_open_read(&sf, inst.tmp, SPLIT_CHUNK_DEFAULT))
        return fail("Fichier téléchargé introuvable sur la carte SD");

    // 3. Archive : on extrait la ROM (ZIP64 géré pour les archives > 4 Go)
    char first_part[260];
    sfile_part_path(inst.tmp, 0, first_part, sizeof(first_part));
    const char *rom = first_part;

    if (is_zip(&sf))
    {
        ret = extract_zip(&sf, inst.tmp2);
        sfile_close(&sf);
        if (ret != 0)
            return -1;
        sfile_remove(inst.tmp); // Libère la place prise par l'archive
        rom = inst.tmp2;
    }
    else
    {
        unsigned int parts = sfile_part_count(&sf);
        sfile_close(&sf);
        // Une ROM DS/DSi fait au plus 512 Mo : elle tient dans un seul morceau
        if (parts > 1)
            return fail("Ce fichier fait plus de 1 Go et n'est pas une archive "
                        ".zip : ce n'est pas une ROM DS/DSi (512 Mo maximum)");
    }

    // 4. Vérification de la ROM
    inst.state = INST_VERIFYING;
    set_msg(inst.verbose ? "Vérification de l'en-tête de la ROM (CRC16)…"
                         : "Vérification de la ROM…", NULL);
    if (!is_nds_rom(rom))
        return fail("Le fichier n'est pas une ROM DS/DSi valide");
    vlog("En-tête de la ROM valide (CRC16 correct)");

    if (!ends_with_ci(inst.name, ".nds") && !ends_with_ci(inst.name, ".dsi"))
    {
        // Ex. "jeu.zip" -> "jeu.nds"
        char *dot = strrchr(inst.name, '.');
        if (dot)
            *dot = '\0';
        size_t len = strlen(inst.name);
        if (len + 5 > sizeof(inst.name))
            inst.name[sizeof(inst.name) - 5] = '\0';
        strcat(inst.name, ".nds");
    }

    // Infos du téléchargement à retenir (avant toute autre requête)
    game_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.source, sizeof(rec.source), "%s", inst.link);
    snprintf(rec.version, sizeof(rec.version), "%s", inst.release);
    snprintf(rec.url, sizeof(rec.url), "%s", inst.url);
    snprintf(rec.etag, sizeof(rec.etag), "%s", inst.etag);
    snprintf(rec.modified, sizeof(rec.modified), "%s", inst.modified);

    // Mise à jour : on garde exactement le même nom de fichier, pour que les
    // sauvegardes (rattachées au nom de la ROM) continuent de fonctionner
    if (inst.updating)
        snprintf(inst.name, sizeof(inst.name), "%s", inst.target.file);
    snprintf(rec.file, sizeof(rec.file), "%s", inst.name);
    snprintf(inst.dest, sizeof(inst.dest), "%s/%s", inst.dir, inst.name);

    if (inst.updating && same_content(rom, inst.dest))
    {
        // Le serveur ne disait pas si le fichier avait changé : il n'a pas changé
        sfile_remove(inst.tmp);
        remove(inst.tmp2);
        games_record(inst.dir, &rec);
        inst.up_to_date = true;
        set_update_result(inst.name, UPD_UP_TO_DATE, inst.release);
        set_msg("%s est déjà à jour", inst.name);
        inst.state = INST_DONE;
        return 0;
    }

    // 5. Mise en place. Lors d'une mise à jour, l'ancienne version est gardée
    //    de côté (.dsishop_old) pour pouvoir revenir en arrière.
    if (inst.updating)
    {
        if (!games_backup(inst.dir, inst.name))
            return fail("Impossible de mettre l'ancienne version de côté");
    }
    else
    {
        remove(inst.dest);
    }

    if (rename(rom, inst.dest) != 0)
    {
        if (inst.updating)
            games_restore_backup(inst.dir, inst.name); // Remet l'ancienne
        return fail("Impossible de renommer le fichier sur la carte SD");
    }

    sfile_remove(inst.tmp);
    remove(inst.tmp2);

    // Retient d'où vient le jeu et l'adresse exacte du téléchargement
    // (affichés dans "Mes jeux" et sur le Remote, utilisés pour les mises à jour)
    if (inst.updating && inst.target.source[0])
        snprintf(rec.source, sizeof(rec.source), "%s", inst.target.source);
    games_record(inst.dir, &rec);
    set_update_result(inst.name, UPD_UP_TO_DATE, inst.release);

    vlog("Copié dans %.70s", inst.dest);
    vlog("Terminé");
    set_msg("%s", inst.updating ? "Mise à jour terminée (sauvegardes conservées)"
                                : "Installation terminée");
    inst.state = INST_DONE;
    return 0;
}

static int install_thread(void *arg)
{
    (void)arg;
    int ret = do_install();
    if (ret != 0)
    {
        // Nettoie les fichiers temporaires en cas d'échec
        sfile_remove(inst.tmp);
        remove(inst.tmp2);
    }
    return ret;
}

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------

// Détail de l'initialisation de la carte SD (affiché dans "À propos" si elle
// n'est pas trouvée)
static char storage_info[160];

const char *installer_storage_info(void)
{
    return storage_info;
}

bool installer_init(void)
{
    const char *argv0 = (__system_argv->argvMagic == ARGV_MAGIC && __system_argv->argc >= 1)
                            ? __system_argv->argv[0] : "(aucun)";

    inst.storage_ok = fatInitDefault();
    int err = errno;

    // fatInitDefault() renvoie "échec" dès qu'un lecteur exigé par le chemin de
    // lancement manque (ex. argv[0] en "fat:/..." sans lecteur de linker), alors
    // que la carte SD interne est déjà montée : on regarde donc directement si
    // "sd:/" est lisible, quoi qu'en dise fatInitDefault().
    if (!inst.storage_ok && isDSiMode())
    {
        DIR *d = opendir("sd:/");
        if (d)
        {
            closedir(d);
            inst.storage_ok = true;
        }
    }

    snprintf(storage_info, sizeof(storage_info), "%s | %s | argv0 : %.80s | errno %d",
             inst.storage_ok ? "SD ok" : "SD absente", isDSiMode() ? "mode DSi" : "mode DS",
             argv0, err);

    // En mode DSi on utilise la carte SD interne, sinon le linker (DLDI)
    snprintf(inst.root, sizeof(inst.root), "%s", isDSiMode() ? "sd:" : "fat:");
    snprintf(inst.dir, sizeof(inst.dir), "%s/roms/nds", inst.root);

    // Verbose : oui tant qu'on ne l'a pas coupé (réglage mémorisé sur la SD)
    char v[8];
    inst.verbose = !(installer_pref_get("verbose", v, sizeof(v)) && v[0] == '0');

    inst.curl_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    inst.state = INST_IDLE;
    return inst.storage_ok;
}

const install_info_t *installer_preview_info(void)
{
    return &inst.info;
}

void installer_confirm(void)
{
    if (inst.state == INST_PREVIEW)
        inst.confirmed = true;
}

bool installer_verbose(void)
{
    return inst.verbose;
}

void installer_set_verbose(bool on)
{
    inst.verbose = on;
    installer_pref_set("verbose", on ? "1" : "0");
}

uint64_t installer_speed(void)
{
    return inst.speed;
}

bool installer_storage_ok(void)
{
    return inst.storage_ok;
}

const char *installer_dir(void)
{
    return inst.dir;
}

const char *installer_root(void)
{
    return inst.root;
}

// Petits réglages enregistrés dans <carte SD>/_dsishop/<nom>
bool installer_pref_get(const char *name, char *out, size_t size)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/_dsishop/%s", inst.root, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

void installer_pref_set(const char *name, const char *value)
{
    char dir[160], path[192];
    snprintf(dir, sizeof(dir), "%s/_dsishop", inst.root);
    mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fputs(value, f);
    fclose(f);
}

bool installer_start(const char *link)
{
    return installer_start_ex(link, false);
}

bool installer_was_remote(void)
{
    return inst.remote;
}

static bool start_thread(void)
{
    // TLS + curl ont besoin d'une pile confortable
    inst.thread = cothread_create(install_thread, NULL, 128 * 1024, 0);
    if (inst.thread < 0)
    {
        fail("Impossible de créer la tâche d'installation");
        return false;
    }
    inst.thread_alive = true;
    return true;
}

static bool free_thread_slot(void)
{
    if (inst.thread_alive)
    {
        if (!cothread_has_joined(inst.thread))
            return false;
        cothread_delete(inst.thread);
        inst.thread_alive = false;
    }
    return true;
}

static bool can_start(void)
{
    if (inst.state >= INST_RESOLVING && inst.state <= INST_VERIFYING)
        return false;
    if (inst.checking || browse_state == BROWSE_LOADING || !inst.storage_ok ||
        !inst.curl_ok || twl.state == TWL_CHECKING || twl.state == TWL_UPDATING ||
        self.state == SELF_CHECKING || self.state == SELF_UPDATING)
        return false;
    return free_thread_slot();
}

static bool installer_start_link(const char *link, bool remote);

bool installer_start_ex(const char *link, bool remote)
{
    if (!can_start())
        return false;
    inst.updating = false;
    return installer_start_link(link, remote);
}

bool installer_update(const char *file, bool remote)
{
    if (!can_start() || !games_valid_name(file))
        return false;

    game_t g;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", inst.dir, file);
    if (!file_exists(path) || !games_get_record(inst.dir, file, &g) ||
        !games_is_tracked(&g))
        return false;

    inst.target = g;
    inst.updating = true;
    return installer_start_link(g.source[0] ? g.source : g.url, remote);
}

// Partie commune : lien nettoyé, puis lancement de la tâche
static bool installer_start_link(const char *link, bool remote)
{
    // Retire les espaces autour du lien
    while (*link == ' ')
        link++;
    snprintf(inst.link, sizeof(inst.link), "%s", link);
    size_t len = strlen(inst.link);
    while (len > 0 && inst.link[len - 1] == ' ')
        inst.link[--len] = '\0';
    if (len == 0)
        return false;

    inst.cancel = false;
    inst.remote = remote;
    inst.now = inst.total = 0;
    inst.confirmed = false;
    inst.start_time = time(NULL);
    log_total = 0;
    vlog("%s : %.60s", inst.updating ? "Mise à jour" : "Installation", inst.link);
    inst.state = INST_RESOLVING;
    set_msg("Préparation…", NULL);
    return start_thread();
}

void installer_cancel(void)
{
    inst.cancel = true;
}

// ---------------------------------------------------------------------------
// Vérification des mises à jour de tous les jeux
// ---------------------------------------------------------------------------

static upd_status_t check_one(const game_t *g, char *latest, size_t latest_size,
                              bool *rate_limited)
{
    latest[0] = '\0';
    if (!games_is_tracked(g))
        return UPD_NOT_TRACKED;

    const char *link = g->source[0] ? g->source : g->url;
    char api_url[320], err[192];

    if (parse_github(link, api_url, sizeof(api_url)) ||
        github_repo_of_download(link, api_url, sizeof(api_url)) ||
        github_repo_of_download(g->url, api_url, sizeof(api_url)))
    {
        char ext[16], url[512], name[128];
        url_ext(g->url, ext, sizeof(ext));
        if (github_latest(api_url, ext, latest, latest_size, url, sizeof(url), name,
                          sizeof(name), err, sizeof(err)) != 0)
        {
            if (strstr(err, "limite"))
                *rate_limited = true;
            return UPD_ERROR;
        }
        // Version installée inconnue (ex. après un retour en arrière)
        if (!g->version[0])
            return UPD_AVAILABLE;
        return strcmp(latest, g->version) == 0 ? UPD_UP_TO_DATE : UPD_AVAILABLE;
    }

    // Lien direct : en-têtes seulement
    if (http_req(g->url[0] ? g->url : link, false, NULL, NULL, err, sizeof(err)) != 0)
        return UPD_ERROR;
    if (g->etag[0] && inst.etag[0])
        return strcmp(g->etag, inst.etag) == 0 ? UPD_UP_TO_DATE : UPD_AVAILABLE;
    if (g->modified[0] && inst.modified[0])
        return strcmp(g->modified, inst.modified) == 0 ? UPD_UP_TO_DATE : UPD_AVAILABLE;
    return UPD_UNKNOWN; // Le serveur ne le dit pas : il faudra télécharger
}

static int check_thread(void *arg)
{
    (void)arg;
    bool rate_limited = false;

    for (int i = 0; i < inst.check_total && !inst.cancel; i++)
    {
        const game_t *g = &check_list[i];
        char latest[64] = "";
        upd_status_t st = rate_limited ? UPD_ERROR
                                       : check_one(g, latest, sizeof(latest), &rate_limited);
        set_update_result(g->file, st, latest);
        inst.check_done = i + 1;
        cothread_yield();
    }

    inst.checking = false;
    return 0;
}

bool installer_check_updates(const game_t *games, int count)
{
    if (!can_start())
        return false;
    if (count > GAMES_MAX)
        count = GAMES_MAX;

    memcpy(check_list, games, sizeof(game_t) * count);
    inst.check_total = count;
    inst.check_done = 0;
    for (int i = 0; i < count; i++)
        set_update_result(games[i].file, UPD_CHECKING, "");

    inst.cancel = false;
    inst.checking = true;
    inst.thread = cothread_create(check_thread, NULL, 128 * 1024, 0);
    if (inst.thread < 0)
    {
        inst.checking = false;
        return false;
    }
    inst.thread_alive = true;
    return true;
}

bool installer_checking(void)
{
    return inst.checking;
}

void installer_check_progress(int *done, int *total)
{
    *done = inst.check_done;
    *total = inst.check_total;
}

// ---------------------------------------------------------------------------
// Parcourir les projets GitHub d'un utilisateur
// ---------------------------------------------------------------------------

// Lit la réponse du catalogue : une ligne par projet, séparée par des tabulations
//   propriétaire <TAB> nom <TAB> étoiles <TAB> tag <TAB> description
static void parse_catalog(char *text)
{
    browse_count = 0;
    char *line = text;
    while (line && *line && browse_count < BROWSE_MAX)
    {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';

        char *f[5] = { NULL };
        int n = 0;
        char *p = line;
        while (n < 5)
        {
            f[n++] = p;
            char *tab = strchr(p, '\t');
            if (!tab)
                break;
            *tab = '\0';
            p = tab + 1;
        }

        if (n >= 4 && f[0][0] && f[1][0])
        {
            browse_repo_t *r = &browse_list[browse_count++];
            snprintf(r->owner, sizeof(r->owner), "%s", f[0]);
            snprintf(r->name, sizeof(r->name), "%s", f[1]);
            r->stars = atoi(f[2]);
            snprintf(r->desc, sizeof(r->desc), "%s", n >= 5 ? f[4] : "");
        }
        line = eol ? eol + 1 : NULL;
    }
}
static int browse_thread(void *arg)
{
    (void)arg;
    char err[192];
    const char *url = browse_url;

    membuf_t mem = { .data = malloc(BROWSE_JSON_MAX), .len = 0, .cap = BROWSE_JSON_MAX };
    if (!mem.data)
    {
        snprintf(browse_msg, sizeof(browse_msg), "Mémoire insuffisante");
        browse_state = BROWSE_ERROR;
        return 0;
    }
    mem.data[0] = '\0';

    if (http_req(url, true, NULL, &mem, err, sizeof(err)) != 0)
    {
        if (strstr(err, "introuvable, ou aucune"))
            snprintf(browse_msg, sizeof(browse_msg), "Utilisateur « %s » introuvable",
                     browse_user);
        else
            snprintf(browse_msg, sizeof(browse_msg), "%.159s", err);
        browse_state = BROWSE_ERROR;
    }
    else
    {
        parse_catalog(mem.data);
        if (browse_count == 0)
        {
            snprintf(browse_msg, sizeof(browse_msg),
                     browse_is_search ? "Aucun jeu trouvé pour « %s »"
                                      : "Aucun projet public pour « %s »",
                     browse_user);
            browse_state = BROWSE_ERROR;
        }
        else
        {
            browse_state = BROWSE_DONE;
        }
    }
    free(mem.data);
    return 0;
}

static bool browse_launch(void)
{
    if (!can_start())
        return false;

    browse_count = 0;
    browse_msg[0] = '\0';
    inst.cancel = false;
    browse_state = BROWSE_LOADING;
    inst.thread = cothread_create(browse_thread, NULL, 128 * 1024, 0);
    if (inst.thread < 0)
    {
        browse_state = BROWSE_ERROR;
        snprintf(browse_msg, sizeof(browse_msg), "Impossible de créer la tâche");
        return false;
    }
    inst.thread_alive = true;
    return true;
}

bool installer_browse_start(const char *user)
{
    // Un nom d'utilisateur GitHub : lettres, chiffres et tirets (1 à 39)
    size_t n = strlen(user);
    if (n == 0 || n > 39)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)user[i]) && user[i] != '-')
            return false;

    snprintf(browse_user, sizeof(browse_user), "%s", user);
    browse_is_search = false;
    snprintf(browse_url, sizeof(browse_url), CATALOG_URL "/user?name=%s&limit=%d", user,
             BROWSE_MAX);
    return browse_launch();
}

// Recherche un jeu par nom parmi les projets DS/DSi de GitHub (sujets nds,
// nintendo-ds, nintendo-dsi, libnds, blocksds), triés par étoiles
bool installer_browse_search(const char *query)
{
    char enc[96];
    size_t n = 0;
    bool any = false;
    for (const char *p = query; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (n + 4 >= sizeof(enc))
            return false;
        if (isalnum(c))
        {
            enc[n++] = (char)c;
            any = true;
        }
        else if (c == ' ' || c == '-' || c == '_' || c == '.')
        {
            if (n > 0 && enc[n - 1] != '+')
                enc[n++] = '+';
        }
        else
            return false;
    }
    while (n > 0 && enc[n - 1] == '+')
        n--;
    enc[n] = '\0';
    if (!any || strlen(query) > 39)
        return false;

    snprintf(browse_user, sizeof(browse_user), "%s", query);
    browse_is_search = true;
    snprintf(browse_url, sizeof(browse_url), CATALOG_URL "/search?q=%s&limit=%d", enc,
             BROWSE_MAX);
    return browse_launch();
}

bool installer_browse_is_search(void) { return browse_is_search; }

browse_state_t installer_browse_state(void) { return browse_state; }
const char *installer_browse_message(void) { return browse_msg; }
const char *installer_browse_user(void) { return browse_user; }
int installer_browse_count(void) { return browse_count; }

const browse_repo_t *installer_browse_get(int index)
{
    return (index >= 0 && index < browse_count) ? &browse_list[index] : NULL;
}

// ---------------------------------------------------------------------------
// Mise à jour de TWiLight Menu++
// ---------------------------------------------------------------------------

// Ce que pèse un paquet dans la barre de progression : téléchargé puis extrait
static uint64_t twl_weight(const sysupd_pack_t *p)
{
    return (uint64_t)p->size + p->unc_size;
}

static bool twl_applies(const sysupd_pack_t *p)
{
    char path[200];
    if (!p->cond[0])
        return true;
    snprintf(path, sizeof(path), "%s/%s", inst.root, p->cond);
    return file_exists(path);
}

static int twl_fetch(void *ud, const char *path, const char *dest_base, char *err,
                     size_t err_size)
{
    (void)ud;
    char url[256];
    snprintf(url, sizeof(url), CATALOG_URL "%s", path);

    sfile_t sf;
    if (!sfile_open_write(&sf, dest_base, SPLIT_CHUNK_DEFAULT))
    {
        snprintf(err, err_size, "Impossible d'écrire sur la carte SD");
        return -1;
    }
    int ret = http_req(url, false, &sf, NULL, err, err_size);
    if (!sfile_close(&sf) && ret == 0)
    {
        snprintf(err, err_size, "Impossible d'écrire sur la carte SD (pleine ?)");
        ret = -1;
    }
    if (ret != 0 && inst.cancel && !inst.no_space)
        snprintf(err, err_size, "Mise à jour annulée");
    return ret;
}

static int twl_progress(void *ud, int pack, sysupd_phase_t phase, uint64_t done,
                        uint64_t total)
{
    (void)ud; (void)total;
    if (pack != twl.pack)
    {
        // Les paquets précédents (appliqués ou sautés) sont derrière nous
        uint64_t before = 0;
        for (int i = twl.resume; i < pack; i++)
            if (twl_applies(&twl.man.packs[i]))
                before += twl_weight(&twl.man.packs[i]);
        twl.done_before = before;
        twl.pack = pack;
    }
    twl.phase = phase;
    twl.pack_done = phase == SYSUPD_EXTRACT ? done : 0;
    if (phase == SYSUPD_DOWNLOAD)
        inst.now = 0;   // Le téléchargement de ce paquet n'a pas commencé
    cothread_yield();
    return inst.cancel ? 1 : 0;
}

static int twl_check_thread(void *arg)
{
    (void)arg;
    char err[192];

    twl.installed[0] = '\0';
    sysupd_installed_version(inst.root, twl.installed, sizeof(twl.installed));

    membuf_t mem = { .data = malloc(BROWSE_JSON_MAX), .len = 0, .cap = BROWSE_JSON_MAX };
    if (!mem.data)
    {
        snprintf(twl.msg, sizeof(twl.msg), "Mémoire insuffisante");
        twl.state = TWL_ERROR;
        return 0;
    }
    mem.data[0] = '\0';

    if (http_req(CATALOG_URL "/twilight/manifest", false, NULL, &mem, err, sizeof(err)) != 0)
    {
        if (strstr(err, "HTTP 503"))
            snprintf(twl.msg, sizeof(twl.msg),
                     "Le serveur prépare la mise à jour : réessaie dans une minute");
        else
            snprintf(twl.msg, sizeof(twl.msg), "%.191s", err);
        twl.state = TWL_ERROR;
    }
    else if (!sysupd_parse_manifest(mem.data, &twl.man))
    {
        snprintf(twl.msg, sizeof(twl.msg), "Réponse du serveur incompréhensible");
        twl.state = TWL_ERROR;
    }
    else
    {
        twl.resume = sysupd_resume_index(inst.root, twl.man.tag);
        if (twl.resume >= twl.man.count)
            twl.resume = 0;
        twl.state = TWL_READY;
    }
    free(mem.data);
    return 0;
}

static int twl_update_thread(void *arg)
{
    (void)arg;
    char err[192] = "";

    uint64_t avail;
    if (free_space(&avail) && avail < twl.man.need)
    {
        char need[24], have[24];
        format_size(need, sizeof(need), twl.man.need);
        format_size(have, sizeof(have), avail);
        snprintf(twl.msg, sizeof(twl.msg),
                 "Pas assez de place sur la carte SD : il faut %s, il reste %s", need, have);
        twl.state = TWL_ERROR;
        return 0;
    }

    sysupd_io_t io = { twl_fetch, twl_progress, NULL };
    if (sysupd_apply(inst.root, &twl.man, &io, err, sizeof(err)) != 0)
    {
        snprintf(twl.msg, sizeof(twl.msg), "%.191s", err);
        twl.resume = sysupd_resume_index(inst.root, twl.man.tag);
        twl.state = TWL_ERROR;
        return 0;
    }
    twl.resume = 0;
    snprintf(twl.installed, sizeof(twl.installed), "%s", twl.man.tag);
    twl.state = TWL_DONE;
    return 0;
}

static bool twl_launch(int (*fn)(void *), twl_state_t state)
{
    if (!can_start())
        return false;
    twl.msg[0] = '\0';
    inst.cancel = false;
    twl.state = state;
    inst.thread = cothread_create(fn, NULL, 128 * 1024, 0);
    if (inst.thread < 0)
    {
        twl.state = TWL_ERROR;
        snprintf(twl.msg, sizeof(twl.msg), "Impossible de créer la tâche");
        return false;
    }
    inst.thread_alive = true;
    return true;
}

bool installer_twl_present(void)
{
    // Les paquets viennent de la version "DSi" : carte SD de la console seulement
    return inst.storage_ok && strcmp(inst.root, "sd:") == 0 && sysupd_present(inst.root);
}

bool installer_twl_check(void)
{
    return twl_launch(twl_check_thread, TWL_CHECKING);
}

bool installer_twl_update(void)
{
    if ((twl.state != TWL_READY && twl.state != TWL_ERROR) || twl.man.count == 0 ||
        !installer_twl_present())
        return false;

    twl.total = 0;
    for (int i = twl.resume; i < twl.man.count; i++)
        if (twl_applies(&twl.man.packs[i]))
            twl.total += twl_weight(&twl.man.packs[i]);
    twl.pack = twl.resume;
    twl.phase = SYSUPD_DOWNLOAD;
    twl.pack_done = 0;
    twl.done_before = 0;
    return twl_launch(twl_update_thread, TWL_UPDATING);
}

twl_state_t installer_twl_state(void) { return twl.state; }
const char *installer_twl_message(void) { return twl.msg; }
const char *installer_twl_installed(void) { return twl.installed; }
const char *installer_twl_latest(void) { return twl.man.tag; }
int installer_twl_resume(void) { return twl.resume; }

void installer_twl_progress(int *pack, int *packs, uint64_t *now, uint64_t *total)
{
    const sysupd_pack_t *p = &twl.man.packs[twl.pack];
    uint64_t cur;
    if (twl.phase == SYSUPD_DOWNLOAD)
        cur = inst.now < p->size ? inst.now : p->size;
    else
        cur = p->size + twl.pack_done;

    *pack = twl.pack + 1;
    *packs = twl.man.count;
    *now = twl.done_before + cur;
    *total = twl.total;
}

// ---------------------------------------------------------------------------
// Lancer DSi Shop au démarrage
// ---------------------------------------------------------------------------

static char self_path[256];

void installer_set_self_path(const char *path)
{
    // Chemin complet sur la carte, ex. "sd:/roms/nds/DSiShop.nds" (c'est aussi
    // sous cette forme que TWiLight Menu++ note la dernière ROM lancée)
    size_t r = strlen(inst.root);
    if (path && r > 0 && strncasecmp(path, inst.root, r) == 0 && path[r] == '/' &&
        ends_with_ci(path, ".nds") && strlen(path) < sizeof(self_path))
        snprintf(self_path, sizeof(self_path), "%s", path);
    else
        self_path[0] = '\0';
}

bool installer_autoboot_available(void)
{
    return self_path[0] != '\0' && installer_twl_present();
}

bool installer_autoboot_get(void)
{
    return installer_autoboot_available() && twlini_autoboot_get(inst.root, self_path);
}

bool installer_autoboot_set(bool on)
{
    return installer_autoboot_available() && twlini_autoboot_set(inst.root, self_path, on);
}

// ---------------------------------------------------------------------------
// Mise à jour de DSi Shop lui-même
// ---------------------------------------------------------------------------

// Projet GitHub qui publie les releases de DSi Shop (fichier DSiShop.nds)
#define SELF_REPO "Rolbubx/DSiShopV2"

static int self_check_thread(void *arg)
{
    (void)arg;
    char err[192], name[128];

    if (github_latest(CATALOG_URL "/api/repos/" SELF_REPO "/releases/latest", ".nds",
                      self.latest, sizeof(self.latest), self.url, sizeof(self.url),
                      name, sizeof(name), err, sizeof(err)) != 0)
    {
        if (strstr(err, "aucune release"))
            snprintf(self.msg, sizeof(self.msg), "Aucune version publiée pour le moment");
        else
            snprintf(self.msg, sizeof(self.msg), "%.191s", err);
        self.state = SELF_ERROR;
        return 0;
    }
    if (!ends_with_ci(name, ".nds"))
    {
        snprintf(self.msg, sizeof(self.msg), "La dernière version ne contient pas de .nds");
        self.state = SELF_ERROR;
        return 0;
    }
    self.newer = sysupd_version_cmp(self.latest, self.current) > 0;
    self.state = SELF_READY;
    return 0;
}

static int self_fail(const char *msg, const char *tmp)
{
    snprintf(self.msg, sizeof(self.msg), "%.191s", msg);
    sfile_remove(tmp);
    self.state = SELF_ERROR;
    return 0;
}

static int self_update_thread(void *arg)
{
    (void)arg;
    char dir[64], tmp[96], part[128], old[96], err[192];

    snprintf(dir, sizeof(dir), "%s/_dsishop", inst.root);
    mkdir_p(dir);
    snprintf(tmp, sizeof(tmp), "%s/self_download", dir);
    snprintf(old, sizeof(old), "%s/DSiShop.old.nds", dir);
    sfile_remove(tmp);

    sfile_t sf;
    if (!sfile_open_write(&sf, tmp, SPLIT_CHUNK_DEFAULT))
        return self_fail("Impossible d'écrire sur la carte SD", tmp);
    int ret = http_req(self.url, false, &sf, NULL, err, sizeof(err));
    unsigned int parts = sfile_part_count(&sf);
    bool write_ok = sfile_close(&sf);
    if (ret != 0)
        return self_fail(inst.cancel && !inst.no_space ? "Mise à jour annulée" : err, tmp);
    if (!write_ok)
        return self_fail("Erreur d'écriture sur la carte SD (pleine ?)", tmp);

    // On ne remplace DSi Shop que par une vraie ROM DS (en-tête vérifié)
    sfile_part_path(tmp, 0, part, sizeof(part));
    if (parts != 1 || !is_nds_rom(part))
        return self_fail("Le fichier téléchargé n'est pas une ROM DS valide", tmp);

    // L'ancienne version est gardée dans _dsishop/DSiShop.old.nds. Si la mise
    // en place échoue, elle est remise à sa place.
    remove(old);
    if (rename(self_path, old) != 0)
        return self_fail("Impossible de remplacer DSi Shop sur la carte SD", tmp);
    if (rename(part, self_path) != 0)
    {
        rename(old, self_path);
        return self_fail("Impossible de remplacer DSi Shop sur la carte SD", tmp);
    }
    self.newer = false;
    self.state = SELF_DONE;
    return 0;
}

static bool self_launch(int (*fn)(void *), self_state_t state)
{
    if (!can_start())
        return false;
    self.msg[0] = '\0';
    inst.cancel = false;
    self.state = state;
    inst.thread = cothread_create(fn, NULL, 128 * 1024, 0);
    if (inst.thread < 0)
    {
        self.state = SELF_ERROR;
        snprintf(self.msg, sizeof(self.msg), "Impossible de créer la tâche");
        return false;
    }
    inst.thread_alive = true;
    return true;
}

bool installer_self_available(void)
{
    return inst.storage_ok && self_path[0] != '\0';
}

bool installer_self_check(const char *current_version)
{
    if (!installer_self_available())
        return false;
    snprintf(self.current, sizeof(self.current), "%s", current_version);
    return self_launch(self_check_thread, SELF_CHECKING);
}

bool installer_self_update(void)
{
    if (!installer_self_available() || !self.url[0] ||
        (self.state != SELF_READY && self.state != SELF_ERROR))
        return false;
    return self_launch(self_update_thread, SELF_UPDATING);
}

self_state_t installer_self_state(void) { return self.state; }
const char *installer_self_message(void) { return self.msg; }
const char *installer_self_latest(void) { return self.latest; }
bool installer_self_newer(void) { return self.newer; }

upd_status_t installer_update_status(const char *file, const char **latest)
{
    for (int i = 0; i < nupd; i++)
    {
        if (strcmp(upd[i].file, file) == 0)
        {
            if (latest)
                *latest = upd[i].latest;
            return upd[i].status;
        }
    }
    if (latest)
        *latest = "";
    return UPD_UNKNOWN;
}

bool installer_last_was_update(void)
{
    return inst.updating;
}

bool installer_up_to_date(void)
{
    return inst.up_to_date;
}

inst_state_t installer_state(void) { return inst.state; }
const char *installer_message(void) { return inst.msg; }
const char *installer_file_name(void) { return inst.name; }
const char *installer_release(void) { return inst.release; }
const char *installer_dest_path(void) { return inst.dest; }
uint64_t installer_progress_now(void) { return inst.now; }
uint64_t installer_progress_total(void) { return inst.total; }

