// Installation de jeux depuis GitHub ou une URL directe

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <curl/curl.h>
#include <fat.h>
#include <nds.h>
#include <zlib.h>

#include "games.h"
#include "installer.h"
#include "splitfile.h"
#include "zip.h"

#define USER_AGENT "DSiShop/0.2 (Nintendo DSi; +https://github.com)"

// Taille max de la réponse JSON de l'API GitHub
#define JSON_MAX (512 * 1024)

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

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

static void set_msg(const char *fmt, const char *arg)
{
    snprintf(inst.msg, sizeof(inst.msg), fmt, arg ? arg : "");
}

static int fail(const char *msg)
{
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
        snprintf(inst.etag, sizeof(inst.etag), "%s", v);
    }
    else if (strncasecmp(line, "last-modified:", 14) == 0)
    {
        const char *v = line + 14;
        while (*v == ' ')
            v++;
        snprintf(inst.modified, sizeof(inst.modified), "%s", v);
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
    blob.len = cacert_pem_size;
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
            ERR("Erreur réseau : %s", curl_easy_strerror(res));
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
        snprintf(api_url, size, "https://api.github.com/repos/%s/%s/releases/tags/%s",
                 owner, repo, tagname);
    }
    else
    {
        // Forme courte sans "github.com" : seulement "auteur/projet" exact
        if (!explicit_host && *p != '\0' && *p != '/')
            return false;
        snprintf(api_url, size, "https://api.github.com/repos/%s/%s/releases/latest",
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
    snprintf(api_url, size, "https://api.github.com/repos/%.*s/releases/latest",
             (int)(dl - p), p);
    return true;
}

// Choisit le meilleur fichier de la release : extension préférée d'abord
// (celle de la version installée, lors d'une mise à jour), puis .nds > .dsi > .zip
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
    set_msg("Recherche de la dernière release sur GitHub…", NULL);

    char err[192];
    if (github_latest(api_url, prefer_ext, inst.release, sizeof(inst.release),
                      inst.url, sizeof(inst.url), inst.name, sizeof(inst.name),
                      err, sizeof(err)) != 0)
        return fail(inst.cancel ? "Installation annulée" : err);
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
    else
        set_msg("Extraction de l'archive…", NULL);

    char name[128] = "";
    zip_err_t err = zip_extract_rom(sf, out_path, name, sizeof(name),
                                    on_zip_progress, NULL);
    if (err != ZIP_OK)
        return fail(zip_strerror(err));

    snprintf(inst.name, sizeof(inst.name), "%s", name);
    return 0;
}

// ---------------------------------------------------------------------------
// Tâche d'installation (cothread)
// ---------------------------------------------------------------------------

static int do_install(void)
{
    inst.release[0] = '\0';
    inst.name[0] = '\0';
    inst.dest[0] = '\0';
    inst.up_to_date = false;

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

    // 2. Téléchargement
    inst.state = INST_DOWNLOADING;
    set_msg("Téléchargement de %s…", inst.name);

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
    set_msg("Vérification de la ROM…", NULL);
    if (!is_nds_rom(rom))
        return fail("Le fichier n'est pas une ROM DS/DSi valide");

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

bool installer_init(void)
{
    inst.storage_ok = fatInitDefault();

    // En mode DSi on utilise la carte SD interne, sinon le linker (DLDI)
    snprintf(inst.root, sizeof(inst.root), "%s", isDSiMode() ? "sd:" : "fat:");
    snprintf(inst.dir, sizeof(inst.dir), "%s/roms/nds", inst.root);

    inst.curl_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    inst.state = INST_IDLE;
    return inst.storage_ok;
}

bool installer_storage_ok(void)
{
    return inst.storage_ok;
}

const char *installer_dir(void)
{
    return inst.dir;
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
    if (inst.checking || !inst.storage_ok || !inst.curl_ok)
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
