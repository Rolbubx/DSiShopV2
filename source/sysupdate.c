// Mise à jour de TWiLight Menu++ par paquets (voir sysupdate.h)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"
#include "splitfile.h"
#include "sysupdate.h"
#include "zip.h"

#define STATE_FILE "_dsishop/twilight.state"
#define PACK_FILE  "_dsishop/twl_pack"

static bool exists(const char *root, const char *rel)
{
    char path[400];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    return stat(path, &st) == 0;
}

// Chemin relatif sans "..", ni lecteur, ni caractère de contrôle
static bool safe_rel(const char *s)
{
    if (!s[0] || s[0] == '/')
        return false;
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == '\\' || *p == ':')
            return false;
    return strstr(s, "..") == NULL;
}

static bool safe_pack_name(const char *s)
{
    size_t n = strlen(s);
    if (n < 5 || n >= sizeof(((sysupd_pack_t *)0)->name) || strcmp(s + n - 4, ".zip") != 0)
        return false;
    for (size_t i = 0; i < n - 4; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return false;
    return true;
}

// Version : lettres, chiffres, ". _ + -" (elle finit dans une URL)
static bool safe_tag(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= sizeof(((sysupd_manifest_t *)0)->tag))
        return false;
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '+' || c == '-'))
            return false;
    }
    return true;
}

// Découpe une ligne en champs séparés par des tabulations
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max)
    {
        f[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab)
            break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

bool sysupd_parse_manifest(char *text, sysupd_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    int expected = -1;
    char *line = text;

    while (line && *line)
    {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';
        line[strcspn(line, "\r")] = '\0';

        char *f[5];
        int n = split_tabs(line, f, 5);

        if (expected < 0)
        {
            if (n != 4 || strcmp(f[0], "twl1") != 0 || !safe_tag(f[1]))
                return false;
            snprintf(m->tag, sizeof(m->tag), "%s", f[1]);
            expected = atoi(f[2]);
            m->need = strtoull(f[3], NULL, 10);
            if (expected < 1 || expected > SYSUPD_MAX_PACKS)
                return false;
        }
        else if (line[0])
        {
            if (n != 5 || m->count >= expected || !safe_pack_name(f[0]) ||
                strlen(f[3]) != 64 || strspn(f[3], "0123456789abcdef") != 64)
                return false;
            sysupd_pack_t *p = &m->packs[m->count];
            snprintf(p->name, sizeof(p->name), "%s", f[0]);
            p->size = (uint32_t)strtoul(f[1], NULL, 10);
            p->unc_size = (uint32_t)strtoul(f[2], NULL, 10);
            snprintf(p->sha, sizeof(p->sha), "%s", f[3]);
            if (strcmp(f[4], "-") != 0)
            {
                if (!safe_rel(f[4]) || strlen(f[4]) >= sizeof(p->cond))
                    return false;
                snprintf(p->cond, sizeof(p->cond), "%s", f[4]);
            }
            if (p->size == 0)
                return false;
            m->count++;
        }
        line = eol ? eol + 1 : NULL;
    }
    return expected > 0 && m->count == expected;
}

bool sysupd_present(const char *root)
{
    return exists(root, "_nds/TWiLightMenu/main.srldr") && exists(root, "BOOT.NDS");
}

bool sysupd_installed_version(const char *root, char *out, size_t size)
{
    // 1re ligne de version.txt : "TWiLight Menu++: v27.25.1"
    char path[300], line[96];
    snprintf(path, sizeof(path), "%s/version.txt", root);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[n] = '\0';
    line[strcspn(line, "\r\n")] = '\0';

    const char *v = strstr(line, ": ");
    if (strncmp(line, "TWiLight Menu++", 15) != 0 || !v || !v[2])
        return false;
    snprintf(out, size, "%s", v + 2);
    return true;
}

int sysupd_resume_index(const char *root, const char *tag)
{
    char path[300], buf[96];
    snprintf(path, sizeof(path), "%s/" STATE_FILE, root);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    // "version <TAB> numéro du prochain paquet"
    char *tab = strchr(buf, '\t');
    if (!tab)
        return 0;
    *tab = '\0';
    int idx = atoi(tab + 1);
    return strcmp(buf, tag) == 0 && idx > 0 ? idx : 0;
}

static bool save_state(const char *root, const char *tag, int next)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/" STATE_FILE, root);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "%s\t%d\n", tag, next);
    return fclose(f) == 0;
}

int sysupd_version_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V')
        a++;
    if (*b == 'v' || *b == 'V')
        b++;
    while (*a || *b)
    {
        unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        // Passe au nombre suivant (séparateur quelconque)
        while (*a && (*a < '0' || *a > '9'))
            a++;
        while (*b && (*b < '0' || *b > '9'))
            b++;
    }
    return 0;
}

#define FAIL(...) do { snprintf(err, err_size, __VA_ARGS__); return -1; } while (0)

typedef struct {
    const sysupd_io_t *io;
    int pack;
} zip_ctx_t;

static int on_zip(void *ud, uint64_t done, uint64_t total)
{
    zip_ctx_t *z = ud;
    return z->io->progress ? z->io->progress(z->io->ud, z->pack, SYSUPD_EXTRACT, done, total) : 0;
}

// Vérifie la taille et le SHA-256 du paquet téléchargé
static int verify_pack(sfile_t *sf, const sysupd_pack_t *p, const sysupd_io_t *io, int index,
                       char *err, size_t err_size)
{
    if (sf->size != p->size)
        FAIL("Paquet %s incomplet : réessaie", p->name);

    const size_t CHUNK = 16 * 1024;
    uint8_t *buf = malloc(CHUNK);
    if (!buf)
        FAIL("Mémoire insuffisante");

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint64_t done = 0;
    int ret = 0;
    sfile_seek(sf, 0);
    while (done < sf->size)
    {
        size_t want = sf->size - done < CHUNK ? (size_t)(sf->size - done) : CHUNK;
        if (sfile_read(sf, buf, want) != want)
        {
            snprintf(err, err_size, "Impossible de relire le paquet %s", p->name);
            ret = -1;
            break;
        }
        sha256_update(&ctx, buf, want);
        done += want;
        if (io->progress && io->progress(io->ud, index, SYSUPD_VERIFY, done, sf->size))
        {
            snprintf(err, err_size, "Mise à jour annulée");
            ret = -1;
            break;
        }
    }
    free(buf);
    if (ret != 0)
        return ret;

    uint8_t hash[32];
    char hex[65];
    sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    if (strcmp(hex, p->sha) != 0)
        FAIL("Paquet %s corrompu pendant le téléchargement : réessaie", p->name);
    return 0;
}

int sysupd_apply(const char *root, const sysupd_manifest_t *m, const sysupd_io_t *io,
                 char *err, size_t err_size)
{
    char dir[300], base[300], url[160];
    snprintf(dir, sizeof(dir), "%s/_dsishop", root);
    mkdir(dir, 0777);
    snprintf(base, sizeof(base), "%s/" PACK_FILE, root);

    int start = sysupd_resume_index(root, m->tag);
    if (start > m->count)
        start = 0;

    for (int i = start; i < m->count; i++)
    {
        const sysupd_pack_t *p = &m->packs[i];

        if (!p->cond[0] || exists(root, p->cond))
        {
            if (io->progress && io->progress(io->ud, i, SYSUPD_DOWNLOAD, 0, p->size))
                FAIL("Mise à jour annulée");

            snprintf(url, sizeof(url), "/twilight/pack/%s/%s", m->tag, p->name);
            sfile_remove(base);
            if (io->fetch(io->ud, url, base, err, err_size) != 0)
            {
                sfile_remove(base);
                return -1;
            }

            sfile_t sf;
            if (!sfile_open_read(&sf, base, SPLIT_CHUNK_DEFAULT))
                FAIL("Impossible de relire le paquet %s", p->name);

            int ret = verify_pack(&sf, p, io, i, err, err_size);
            if (ret == 0)
            {
                zip_ctx_t z = { io, i };
                zip_err_t zerr = zip_extract_all(&sf, root, on_zip, &z);
                if (zerr == ZIP_ERR_CANCELLED)
                {
                    snprintf(err, err_size, "Mise à jour annulée");
                    ret = -1;
                }
                else if (zerr != ZIP_OK)
                {
                    snprintf(err, err_size, "Paquet %s : %s", p->name, zip_strerror(zerr));
                    ret = -1;
                }
            }
            sfile_close(&sf);
            sfile_remove(base);
            if (ret != 0)
                return ret;
        }

        // Paquet appliqué : en cas de coupure, on reprendra au suivant
        if (i + 1 < m->count && !save_state(root, m->tag, i + 1))
            FAIL("Impossible d'écrire sur la carte SD (pleine ?)");
    }

    snprintf(base, sizeof(base), "%s/" STATE_FILE, root);
    remove(base);
    return 0;
}
