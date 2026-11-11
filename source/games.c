// Jeux installés (voir games.h)

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "games.h"

#define MANIFEST ".dsishop.tsv"

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return lx <= ls && strcasecmp(s + ls - lx, suffix) == 0;
}

bool games_valid_name(const char *file)
{
    if (!file || !file[0] || strlen(file) >= sizeof(((game_t *)0)->file))
        return false;
    if (strchr(file, '/') || strchr(file, '\\') || strchr(file, ':') ||
        strchr(file, '\t') || strchr(file, '\n') || strstr(file, ".."))
        return false;
    return ends_with_ci(file, ".nds") || ends_with_ci(file, ".dsi");
}

static void copy_field(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    for (; src && *src && n + 1 < size; src++)
        dst[n++] = (*src == '\t' || *src == '\n' || *src == '\r') ? ' ' : *src;
    dst[n] = '\0';
}

// Lit le registre : appelle cb pour chaque ligne "fichier\tsource\tversion"
typedef void (*manifest_cb)(void *ud, char *file, char *source, char *version);

static void read_manifest(const char *dir, manifest_cb cb, void *ud)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, MANIFEST);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *file = line;
        char *source = strchr(file, '\t');
        if (!source)
            continue;
        *source++ = '\0';
        char *version = strchr(source, '\t');
        if (version)
            *version++ = '\0';
        else
            version = "";
        cb(ud, file, source, version);
    }
    fclose(f);
}

typedef struct {
    game_t *games;
    int count;
} scan_ctx_t;

static void apply_manifest(void *ud, char *file, char *source, char *version)
{
    scan_ctx_t *ctx = ud;
    for (int i = 0; i < ctx->count; i++)
    {
        if (strcmp(ctx->games[i].file, file) == 0)
        {
            copy_field(ctx->games[i].source, sizeof(ctx->games[i].source), source);
            copy_field(ctx->games[i].version, sizeof(ctx->games[i].version), version);
        }
    }
}

static int cmp_games(const void *a, const void *b)
{
    return strcasecmp(((const game_t *)a)->file, ((const game_t *)b)->file);
}

int games_scan(const char *dir, game_t *out, int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max)
    {
        if (e->d_name[0] == '.' || !games_valid_name(e->d_name))
            continue;

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        game_t *g = &out[n++];
        memset(g, 0, sizeof(*g));
        snprintf(g->file, sizeof(g->file), "%.127s", e->d_name); // Longueur déjà vérifiée
        g->size = (uint64_t)st.st_size;
    }
    closedir(d);

    qsort(out, n, sizeof(game_t), cmp_games);

    scan_ctx_t ctx = { out, n };
    read_manifest(dir, apply_manifest, &ctx);
    return n;
}

// Réécrit le registre en remplaçant (ou supprimant) l'entrée d'un fichier
typedef struct {
    FILE *out;
    const char *skip;
} rewrite_ctx_t;

static void copy_other(void *ud, char *file, char *source, char *version)
{
    rewrite_ctx_t *ctx = ud;
    if (strcmp(file, ctx->skip) != 0)
        fprintf(ctx->out, "%s\t%s\t%s\n", file, source, version);
}

static bool rewrite_manifest(const char *dir, const char *file,
                             const char *source, const char *version)
{
    char path[320], tmp[330];
    snprintf(path, sizeof(path), "%s/%s", dir, MANIFEST);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *out = fopen(tmp, "w");
    if (!out)
        return false;

    rewrite_ctx_t ctx = { out, file };
    read_manifest(dir, copy_other, &ctx);

    if (source)
    {
        char s[256], v[64];
        copy_field(s, sizeof(s), source);
        copy_field(v, sizeof(v), version);
        fprintf(out, "%s\t%s\t%s\n", file, s, v);
    }

    bool ok = fclose(out) == 0;
    if (ok)
    {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

bool games_record(const char *dir, const char *file, const char *source,
                  const char *version)
{
    if (!games_valid_name(file))
        return false;
    return rewrite_manifest(dir, file, source ? source : "", version ? version : "");
}

bool games_uninstall(const char *dir, const char *file)
{
    if (!games_valid_name(file))
        return false;

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (remove(path) != 0)
        return false;

    rewrite_manifest(dir, file, NULL, NULL);
    return true;
}
