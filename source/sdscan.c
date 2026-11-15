// Recherche des jeux sur toute la carte SD (voir sdscan.h)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "sdscan.h"

// Dossiers qui ne contiennent pas de jeux à gérer
static const char *const SKIP_DIRS[] = {
    "_nds", "_dsishop", "title", "private", "dcim", "photo", "luma", "3ds",
    "nintendo 3ds", "hiya", "system volume information", NULL,
};

static bool skip_dir(const char *name)
{
    if (name[0] == '.' || name[0] == '$')
        return true;
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (strcasecmp(name, SKIP_DIRS[i]) == 0)
            return true;
    return false;
}

// Même règle que games.c : .nds ou .dsi, nom de moins de 128 octets
static bool is_game_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n >= 128 || name[0] == '.')
        return false;
    return strcasecmp(name + n - 4, ".nds") == 0 || strcasecmp(name + n - 4, ".dsi") == 0;
}

static int cmp_games(const void *a, const void *b)
{
    const sdgame_t *x = a, *y = b;
    int c = strcasecmp(sdgame_name(x), sdgame_name(y));
    return c ? c : strcmp(x->path, y->path);
}

void sdscan_cancel(sdscan_t *s)
{
    for (int i = 0; i < SDSCAN_DEPTH; i++)
    {
        if (s->dir[i])
            closedir(s->dir[i]);
        s->dir[i] = NULL;
    }
    s->depth = -1;
    s->done = true;
}

void sdscan_start(sdscan_t *s, const char *root)
{
    sdscan_cancel(s);       // Referme une analyse précédente restée ouverte
    memset(s, 0, sizeof(*s));

    snprintf(s->path, sizeof(s->path), "%s", root);
    size_t n = strlen(s->path);
    while (n > 0 && s->path[n - 1] == '/')
        s->path[--n] = '\0';
    s->plen[0] = n;

    s->dir[0] = opendir(root);
    s->depth = s->dir[0] ? 0 : -1;
    s->dirs = s->dir[0] ? 1 : 0;
    s->done = s->dir[0] == NULL;
}

bool sdscan_step(sdscan_t *s, int budget)
{
    while (!s->done && budget-- > 0)
    {
        struct dirent *e = readdir(s->dir[s->depth]);
        if (!e)
        {
            // Dossier fini : on remonte
            closedir(s->dir[s->depth]);
            s->dir[s->depth] = NULL;
            s->depth--;
            if (s->depth < 0)
            {
                s->done = true;
                qsort(s->games, (size_t)s->count, sizeof(sdgame_t), cmp_games);
                break;
            }
            s->path[s->plen[s->depth]] = '\0';
            continue;
        }

        const char *name = e->d_name;
        if (name[0] == '.' || name[0] == '$')
            continue;
        size_t base = s->plen[s->depth], nlen = strlen(name);
        if (base + 1 + nlen >= sizeof(s->path))
            continue;                       // Chemin trop long : on l'ignore

        s->path[base] = '/';
        memcpy(s->path + base + 1, name, nlen + 1);

        unsigned char type = e->d_type;
        struct stat st;
        bool have_stat = false;
        if (type != DT_DIR && type != DT_REG)   // Type inconnu : on le demande
        {
            have_stat = stat(s->path, &st) == 0;
            type = !have_stat ? 0 : S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : 0;
        }

        if (type == DT_DIR)
        {
            if (!skip_dir(name) && s->depth + 1 < SDSCAN_DEPTH)
            {
                DIR *d = opendir(s->path);
                if (d)
                {
                    s->depth++;
                    s->dir[s->depth] = d;
                    s->plen[s->depth] = base + 1 + nlen;
                    s->dirs++;
                    continue;               // On garde le chemin de ce dossier
                }
            }
        }
        else if (type == DT_REG && is_game_name(name) && stat(s->path, &st) == 0 &&
                 S_ISREG(st.st_mode))
        {
            if (s->count >= SDSCAN_MAX)
            {
                s->truncated = true;
                sdscan_cancel(s);
                qsort(s->games, (size_t)s->count, sizeof(sdgame_t), cmp_games);
                break;
            }
            sdgame_t *g = &s->games[s->count++];
            snprintf(g->path, sizeof(g->path), "%s", s->path);
            g->size = (uint64_t)st.st_size;
        }
        s->path[base] = '\0';
    }
    return s->done;
}

const char *sdgame_name(const sdgame_t *g)
{
    const char *slash = strrchr(g->path, '/');
    return slash ? slash + 1 : g->path;
}

void sdgame_folder(const sdgame_t *g, char *out, size_t size)
{
    const char *slash = strrchr(g->path, '/');
    size_t n = slash ? (size_t)(slash - g->path) : 0;
    if (n >= size)
        n = size - 1;
    memcpy(out, g->path, n);
    out[n] = '\0';
}
