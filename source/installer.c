// Installation de jeux depuis GitHub ou une URL directe

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <fat.h>
#include <nds.h>
#include <zlib.h>

#include "installer.h"

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

    char root[8];          // "sd:" ou "fat:"
    char dir[64];          // Dossier d'installation
    char link[256];        // Lien saisi par l'utilisateur

    char msg[192];
    char url[512];         // URL du fichier à télécharger
    char name[128];        // Nom du fichier
    char release[64];
    char dest[256];
    char tmp[256];
    char tmp2[256];

    size_t now, total;

    cothread_t thread;
    bool thread_alive;
} inst;

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

static bool is_zip(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t sig[4] = { 0 };
    size_t n = fread(sig, 1, 4, f);
    fclose(f);
    return n == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
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

static size_t write_file(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

static int progress_cb(void *p, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)p; (void)ultotal; (void)ulnow;
    inst.now = (size_t)dlnow;
    inst.total = (size_t)dltotal;
    if (inst.cancel)
        return 1;
    cothread_yield(); // Laisse l'interface se mettre à jour
    return 0;
}

// Effectue un GET. Écrit dans un fichier (file != NULL) ou en mémoire.
static int http_get(const char *url, bool github_api, FILE *file, membuf_t *mem)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return fail("Erreur interne : curl_easy_init()");

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

    // Vérification des certificats HTTPS (on installe des exécutables !)
    char custom[64];
    snprintf(custom, sizeof(custom), "%s%s", inst.root, CUSTOM_CACERT);
    struct curl_blob blob = {
        .data = (void *)cacert_pem,
        .len = cacert_pem_size,
        .flags = CURL_BLOB_NOCOPY,
    };
    if (file_exists(custom))
        curl_easy_setopt(curl, CURLOPT_CAINFO, custom);
    else
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (file)
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

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK)
        return 0;

    char buf[160];
    switch (res)
    {
        case CURLE_ABORTED_BY_CALLBACK:
            return fail("Installation annulée");
        case CURLE_COULDNT_RESOLVE_HOST:
            return fail("Serveur introuvable (DNS). Vérifie le lien et la connexion.");
        case CURLE_COULDNT_CONNECT:
            return fail("Impossible de joindre le serveur");
        case CURLE_OPERATION_TIMEDOUT:
            return fail("Le serveur ne répond plus (délai dépassé)");
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
            return fail("Certificat HTTPS refusé. Vérifie la date et l'heure de "
                        "la console dans les paramètres système.");
        case CURLE_WRITE_ERROR:
            return fail(file ? "Impossible d'écrire sur la carte SD (pleine ?)"
                             : "Réponse du serveur trop volumineuse");
        case CURLE_HTTP_RETURNED_ERROR:
            if (code == 404)
                return fail(github_api ? "Projet introuvable, ou aucune release publiée"
                                       : "Fichier introuvable (erreur 404)");
            if (code == 403 && github_api)
                return fail("GitHub limite les requêtes : réessaie dans une heure");
            snprintf(buf, sizeof(buf), "Le serveur a répondu avec l'erreur HTTP %ld", code);
            return fail(buf);
        default:
            snprintf(buf, sizeof(buf), "Erreur réseau : %s", curl_easy_strerror(res));
            return fail(buf);
    }
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

// Choisit le meilleur fichier de la release : .nds > .dsi > .zip
static int pick_asset(const char *json)
{
    static const char *const exts[] = { ".nds", ".dsi", ".zip" };
    char url[512];

    json_string_after(json, "\"tag_name\"", inst.release, sizeof(inst.release));

    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++)
    {
        const char *pos = json;
        while ((pos = json_string_after(pos, "\"browser_download_url\"", url,
                                        sizeof(url))) != NULL)
        {
            char name[128];
            basename_from_url(url, name, sizeof(name));
            if (ends_with_ci(name, exts[e]))
            {
                snprintf(inst.url, sizeof(inst.url), "%s", url);
                snprintf(inst.name, sizeof(inst.name), "%s", name);
                return 0;
            }
        }
    }

    return fail("La release ne contient aucun fichier .nds, .dsi ou .zip");
}

static int resolve_github(const char *api_url)
{
    inst.state = INST_RESOLVING;
    set_msg("Recherche de la dernière release sur GitHub…", NULL);

    membuf_t mem = { .data = malloc(JSON_MAX), .len = 0, .cap = JSON_MAX };
    if (!mem.data)
        return fail("Mémoire insuffisante");
    mem.data[0] = '\0';

    int ret = http_get(api_url, true, NULL, &mem);
    if (ret == 0)
        ret = pick_asset(mem.data);

    free(mem.data);
    return ret;
}

// ---------------------------------------------------------------------------
// Extraction ZIP (méthodes "stored" et "deflate")
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int extract_zip(const char *zip_path, const char *out_path)
{
    inst.state = INST_EXTRACTING;
    set_msg("Extraction de l'archive…", NULL);

    FILE *f = fopen(zip_path, "rb");
    if (!f)
        return fail("Impossible d'ouvrir l'archive téléchargée");

    // Cherche la fin du répertoire central (dans les 64 derniers Ko)
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    uint8_t *buf = malloc(tail);
    if (!buf)
    {
        fclose(f);
        return fail("Mémoire insuffisante");
    }
    fseek(f, size - tail, SEEK_SET);
    if (fread(buf, 1, tail, f) != (size_t)tail)
        tail = 0;

    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
    {
        if (rd32(buf + i) == 0x06054b50)
        {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
    {
        free(buf);
        fclose(f);
        return fail("Archive .zip invalide");
    }

    uint16_t entries = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12);
    uint32_t cd_offset = rd32(buf + eocd + 16);
    free(buf);

    uint8_t *cd = malloc(cd_size);
    if (!cd)
    {
        fclose(f);
        return fail("Mémoire insuffisante");
    }
    fseek(f, cd_offset, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size)
    {
        free(cd);
        fclose(f);
        return fail("Archive .zip tronquée");
    }

    // Cherche la ROM : .nds en priorité, sinon .dsi
    long best = -1;
    int best_rank = 99;
    char best_name[128] = "";
    uint32_t off = 0;
    for (int i = 0; i < entries && off + 46 <= cd_size; i++)
    {
        const uint8_t *e = cd + off;
        if (rd32(e) != 0x02014b50)
            break;
        uint16_t nlen = rd16(e + 28), xlen = rd16(e + 30), clen = rd16(e + 32);

        char name[256];
        size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, e + 46, n);
        name[n] = '\0';

        int rank = 99;
        if (strstr(name, "__MACOSX") == NULL)
        {
            if (ends_with_ci(name, ".nds"))
                rank = 0;
            else if (ends_with_ci(name, ".dsi"))
                rank = 1;
        }
        if (rank < best_rank)
        {
            best_rank = rank;
            best = off;
            basename_from_url(name, best_name, sizeof(best_name));
        }

        off += 46 + nlen + xlen + clen;
    }

    if (best < 0)
    {
        free(cd);
        fclose(f);
        return fail("Aucun fichier .nds ou .dsi dans l'archive");
    }

    const uint8_t *e = cd + best;
    uint16_t method = rd16(e + 10);
    uint32_t crc_expected = rd32(e + 16);
    uint32_t comp_size = rd32(e + 20);
    uint32_t uncomp_size = rd32(e + 24);
    uint32_t local_off = rd32(e + 42);
    free(cd);

    snprintf(inst.name, sizeof(inst.name), "%s", best_name);

    if (method != 0 && method != 8)
    {
        fclose(f);
        return fail("Méthode de compression .zip non supportée");
    }

    uint8_t lh[30];
    fseek(f, local_off, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50)
    {
        fclose(f);
        return fail("Archive .zip corrompue");
    }
    fseek(f, local_off + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);

    FILE *out = fopen(out_path, "wb");
    if (!out)
    {
        fclose(f);
        return fail("Impossible d'écrire sur la carte SD");
    }

    const size_t CHUNK = 16 * 1024;
    uint8_t *in_buf = malloc(CHUNK), *out_buf = malloc(CHUNK);
    z_stream zs = { 0 };
    bool ok = in_buf && out_buf;
    if (ok && method == 8)
        ok = inflateInit2(&zs, -MAX_WBITS) == Z_OK;

    uint32_t remaining = comp_size;
    uint32_t written = 0, crc = crc32(0, Z_NULL, 0);
    int zret = Z_OK;
    inst.now = 0;
    inst.total = uncomp_size;

    while (ok && remaining > 0 && zret != Z_STREAM_END)
    {
        if (inst.cancel)
        {
            ok = false;
            break;
        }

        size_t want = remaining < CHUNK ? remaining : CHUNK;
        size_t got = fread(in_buf, 1, want, f);
        if (got == 0)
        {
            ok = false;
            break;
        }
        remaining -= got;

        if (method == 0)
        {
            crc = crc32(crc, in_buf, got);
            ok = fwrite(in_buf, 1, got, out) == got;
            written += got;
        }
        else
        {
            zs.next_in = in_buf;
            zs.avail_in = got;
            do
            {
                zs.next_out = out_buf;
                zs.avail_out = CHUNK;
                zret = inflate(&zs, Z_NO_FLUSH);
                if (zret != Z_OK && zret != Z_STREAM_END)
                {
                    ok = false;
                    break;
                }
                size_t have = CHUNK - zs.avail_out;
                crc = crc32(crc, out_buf, have);
                if (fwrite(out_buf, 1, have, out) != have)
                    ok = false;
                written += have;
            } while (ok && zs.avail_out == 0 && zret != Z_STREAM_END);
        }

        inst.now = written;
        cothread_yield();
    }

    if (method == 8)
        inflateEnd(&zs);
    free(in_buf);
    free(out_buf);
    fclose(out);
    fclose(f);

    if (inst.cancel)
        return fail("Installation annulée");
    if (!ok || written != uncomp_size || crc != crc_expected)
        return fail("Échec de l'extraction (archive corrompue ou SD pleine)");

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

    mkdir_p(inst.dir);
    snprintf(inst.tmp, sizeof(inst.tmp), "%s/.dsishop_download.tmp", inst.dir);
    snprintf(inst.tmp2, sizeof(inst.tmp2), "%s/.dsishop_extract.tmp", inst.dir);

    // 1. Lien GitHub -> URL du fichier de la dernière release
    char api_url[320];
    if (parse_github(inst.link, api_url, sizeof(api_url)))
    {
        if (resolve_github(api_url) != 0)
            return -1;
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

    FILE *f = fopen(inst.tmp, "wb");
    if (!f)
        return fail("Impossible d'écrire sur la carte SD");
    setvbuf(f, NULL, _IOFBF, 32 * 1024);
    int ret = http_get(inst.url, false, f, NULL);
    if (fclose(f) != 0 && ret == 0)
        return fail("Erreur d'écriture sur la carte SD");
    if (ret != 0)
        return -1;

    // 3. Archive : on extrait la ROM
    const char *rom = inst.tmp;
    if (is_zip(inst.tmp))
    {
        if (extract_zip(inst.tmp, inst.tmp2) != 0)
            return -1;
        remove(inst.tmp);
        rom = inst.tmp2;
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

    // 5. Mise en place (remplace l'ancienne version)
    snprintf(inst.dest, sizeof(inst.dest), "%s/%s", inst.dir, inst.name);
    remove(inst.dest);
    if (rename(rom, inst.dest) != 0)
        return fail("Impossible de renommer le fichier sur la carte SD");

    remove(inst.tmp);
    remove(inst.tmp2);

    set_msg("%s", "Installation terminée");
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
        remove(inst.tmp);
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
    if (inst.state >= INST_RESOLVING && inst.state <= INST_VERIFYING)
        return false;
    if (!inst.storage_ok || !inst.curl_ok)
        return false;

    if (inst.thread_alive)
    {
        if (!cothread_has_joined(inst.thread))
            return false;
        cothread_delete(inst.thread);
        inst.thread_alive = false;
    }

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
    inst.now = inst.total = 0;
    inst.state = INST_RESOLVING;
    set_msg("Préparation…", NULL);

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

void installer_cancel(void)
{
    inst.cancel = true;
}

inst_state_t installer_state(void) { return inst.state; }
const char *installer_message(void) { return inst.msg; }
const char *installer_file_name(void) { return inst.name; }
const char *installer_release(void) { return inst.release; }
const char *installer_dest_path(void) { return inst.dest; }
size_t installer_progress_now(void) { return inst.now; }
size_t installer_progress_total(void) { return inst.total; }
