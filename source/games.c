// Jeux installés (voir games.h)

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "games.h"

#define MANIFEST   ".dsishop.tsv"
#define BACKUP_DIR ".dsishop_old"

// Champs du registre, dans l'ordre des colonnes
#define FIELDS 6

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

bool games_is_tracked(const game_t *g)
{
    return g->source[0] != '\0' || g->url[0] != '\0';
}

static void copy_field(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    for (; src && *src && n + 1 < size; src++)
        dst[n++] = (*src == '\t' || *src == '\n' || *src == '\r') ? ' ' : *src;
    dst[n] = '\0';
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// ---------------------------------------------------------------------------
// Registre : une ligne par jeu
//   fichier \t source \t version \t url \t etag \t modified
// (les anciennes lignes à 3 colonnes restent lisibles)
// ---------------------------------------------------------------------------

typedef void (*manifest_cb)(void *ud, char **f);

static void read_manifest(const char *dir, manifest_cb cb, void *ud)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, MANIFEST);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[1536]; // Toutes les colonnes de game_t + tabulations
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[FIELDS];
        char *s = line;
        int n = 0;
        while (n < FIELDS)
        {
            fields[n++] = s;
            char *tab = strchr(s, '\t');
            if (!tab)
                break;
            *tab = '\0';
            s = tab + 1;
        }
        if (n < 2)
            continue;
        for (int i = n; i < FIELDS; i++)
            fields[i] = "";
        cb(ud, fields);
    }
    fclose(f);
}

static void fill_from_fields(game_t *g, char **f)
{
    copy_field(g->source, sizeof(g->source), f[1]);
    copy_field(g->version, sizeof(g->version), f[2]);
    copy_field(g->url, sizeof(g->url), f[3]);
    copy_field(g->etag, sizeof(g->etag), f[4]);
    copy_field(g->modified, sizeof(g->modified), f[5]);
}

typedef struct {
    game_t *games;
    int count;
} scan_ctx_t;

static void apply_manifest(void *ud, char **f)
{
    scan_ctx_t *ctx = ud;
    for (int i = 0; i < ctx->count; i++)
        if (strcmp(ctx->games[i].file, f[0]) == 0)
            fill_from_fields(&ctx->games[i], f);
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

// Réécrit le registre en remplaçant (ou supprimant si g == NULL) une entrée
typedef struct {
    FILE *out;
    const char *skip;
} rewrite_ctx_t;

static void write_line(FILE *out, char **f)
{
    fprintf(out, "%s\t%s\t%s\t%s\t%s\t%s\n", f[0], f[1], f[2], f[3], f[4], f[5]);
}

static void copy_other(void *ud, char **f)
{
    rewrite_ctx_t *ctx = ud;
    if (strcmp(f[0], ctx->skip) != 0)
        write_line(ctx->out, f);
}

static bool rewrite_manifest(const char *dir, const char *file, const game_t *g)
{
    char path[320], tmp[330];
    snprintf(path, sizeof(path), "%s/%s", dir, MANIFEST);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *out = fopen(tmp, "w");
    if (!out)
        return false;

    rewrite_ctx_t ctx = { out, file };
    read_manifest(dir, copy_other, &ctx);

    if (g)
    {
        game_t c;
        memset(&c, 0, sizeof(c));
        copy_field(c.source, sizeof(c.source), g->source);
        copy_field(c.version, sizeof(c.version), g->version);
        copy_field(c.url, sizeof(c.url), g->url);
        copy_field(c.etag, sizeof(c.etag), g->etag);
        copy_field(c.modified, sizeof(c.modified), g->modified);
        char *f[FIELDS] = { (char *)file, c.source, c.version, c.url, c.etag, c.modified };
        write_line(out, f);
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

typedef struct {
    const char *file;
    game_t *out;
    bool found;
} find_ctx_t;

static void find_entry(void *ud, char **f)
{
    find_ctx_t *ctx = ud;
    if (strcmp(f[0], ctx->file) == 0)
    {
        fill_from_fields(ctx->out, f);
        ctx->found = true;
    }
}

bool games_get_record(const char *dir, const char *file, game_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->file, sizeof(out->file), "%s", file);
    find_ctx_t ctx = { file, out, false };
    read_manifest(dir, find_entry, &ctx);
    return ctx.found;
}

bool games_record(const char *dir, const game_t *g)
{
    if (!games_valid_name(g->file))
        return false;
    return rewrite_manifest(dir, g->file, g);
}

// ---------------------------------------------------------------------------
// Version précédente
// ---------------------------------------------------------------------------

static void backup_path(const char *dir, const char *file, char *out, size_t size)
{
    snprintf(out, size, "%s/%s/%s", dir, BACKUP_DIR, file);
}

bool games_backup(const char *dir, const char *file)
{
    if (!games_valid_name(file))
        return false;

    char rom[320], bdir[320], bak[400];
    snprintf(rom, sizeof(rom), "%s/%s", dir, file);
    snprintf(bdir, sizeof(bdir), "%s/%s", dir, BACKUP_DIR);
    backup_path(dir, file, bak, sizeof(bak));

    if (!file_exists(rom))
        return false;
    mkdir(bdir, 0777);
    remove(bak); // On ne garde qu'une version précédente
    return rename(rom, bak) == 0;
}

bool games_has_backup(const char *dir, const char *file)
{
    if (!games_valid_name(file))
        return false;
    char bak[400];
    backup_path(dir, file, bak, sizeof(bak));
    return file_exists(bak);
}

bool games_restore_backup(const char *dir, const char *file)
{
    if (!games_has_backup(dir, file))
        return false;

    char rom[320], bak[400], tmp[420];
    snprintf(rom, sizeof(rom), "%s/%s", dir, file);
    backup_path(dir, file, bak, sizeof(bak));
    snprintf(tmp, sizeof(tmp), "%s.swap", bak);

    // Échange : la version actuelle devient la "précédente" (on peut
    // donc revenir à la nouvelle si on change d'avis)
    remove(tmp);
    if (file_exists(rom) && rename(rom, tmp) != 0)
        return false;
    if (rename(bak, rom) != 0)
    {
        rename(tmp, rom); // On remet tout en place
        return false;
    }
    if (file_exists(tmp))
        rename(tmp, bak);

    // La version retenue dans le registre n'est plus la bonne : on l'efface
    // (l'adresse de téléchargement est gardée pour les mises à jour)
    game_t g;
    if (games_get_record(dir, file, &g))
    {
        g.version[0] = '\0';
        g.etag[0] = '\0';
        g.modified[0] = '\0';
        games_record(dir, &g);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fichiers de sauvegarde
// ---------------------------------------------------------------------------

static const char *const SAVE_EXTS[] = {
    ".sav", ".sav1", ".sav2", ".sav3", ".sav4", ".sav5", ".sav6", ".sav7",
    ".sav8", ".sav9", ".pub", ".prv", ".bak",
};

int games_find_saves(const char *dir, const char *file, char paths[][320], int max)
{
    if (!games_valid_name(file))
        return 0;

    // Nom sans extension
    char base[128];
    snprintf(base, sizeof(base), "%s", file);
    char *dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';

    static const char *const SUBDIRS[] = { "", "/saves" };
    int n = 0;
    for (size_t d = 0; d < sizeof(SUBDIRS) / sizeof(SUBDIRS[0]); d++)
    {
        for (size_t e = 0; e < sizeof(SAVE_EXTS) / sizeof(SAVE_EXTS[0]); e++)
        {
            char path[320];
            snprintf(path, sizeof(path), "%s%s/%s%s", dir, SUBDIRS[d], base, SAVE_EXTS[e]);
            if (!file_exists(path))
                continue;
            if (paths && n < max)
                snprintf(paths[n], 320, "%s", path);
            n++;
        }
    }
    return n;
}

int games_delete_saves(const char *dir, const char *file)
{
    // Statique : 5 Ko, trop gros pour la pile (16 Ko sur DS)
    static char paths[GAMES_MAX_SAVES][320];
    int n = games_find_saves(dir, file, paths, GAMES_MAX_SAVES);
    if (n > GAMES_MAX_SAVES)
        n = GAMES_MAX_SAVES;
    int deleted = 0;
    for (int i = 0; i < n; i++)
        if (remove(paths[i]) == 0)
            deleted++;
    return deleted;
}

// ---------------------------------------------------------------------------

bool games_uninstall(const char *dir, const char *file)
{
    if (!games_valid_name(file))
        return false;

    char path[320], bak[400];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (remove(path) != 0)
        return false;

    backup_path(dir, file, bak, sizeof(bak));
    remove(bak);

    rewrite_manifest(dir, file, NULL);
    return true;
}
