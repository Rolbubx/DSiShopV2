// Point de reprise d'un téléchargement (voir dlresume.h)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlresume.h"

#define MAGIC "dsishop-resume 1"

static void put(char *dst, size_t size, const char *value)
{
    snprintf(dst, size, "%s", value);
}

bool dlresume_load(const char *path, dlresume_t *r)
{
    memset(r, 0, sizeof(*r));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    char line[640];
    bool magic = false, have_link = false, have_url = false, have_name = false;
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strcspn(line, "\r\n");
        if (line[n] == '\0')
        {
            // Ligne trop longue (pas de fin de ligne dans le tampon) : fichier abîmé
            if (!feof(f))
            {
                fclose(f);
                return false;
            }
        }
        line[n] = '\0';

        if (!magic)
        {
            magic = strcmp(line, MAGIC) == 0;
            if (!magic)
                break;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        if (!strcmp(key, "link"))         { put(r->link, sizeof(r->link), val); have_link = true; }
        else if (!strcmp(key, "url"))     { put(r->url, sizeof(r->url), val); have_url = true; }
        else if (!strcmp(key, "name"))    { put(r->name, sizeof(r->name), val); have_name = true; }
        else if (!strcmp(key, "release")) put(r->release, sizeof(r->release), val);
        else if (!strcmp(key, "etag"))    put(r->etag, sizeof(r->etag), val);
        else if (!strcmp(key, "total"))   r->total = strtoull(val, NULL, 10);
        else if (!strcmp(key, "done"))    r->done = strtoull(val, NULL, 10);
        else if (!strcmp(key, "complete")) r->complete = val[0] == '1';
    }
    fclose(f);

    if (!magic || !have_link || !have_url || !have_name || (r->total && r->done > r->total))
    {
        memset(r, 0, sizeof(*r));
        return false;
    }
    return true;
}

bool dlresume_save(const char *path, const dlresume_t *r)
{
    // Pas de saut de ligne dans les champs : on ne les écrit pas du tout
    if (strpbrk(r->link, "\r\n") || strpbrk(r->url, "\r\n") || strpbrk(r->name, "\r\n") ||
        strpbrk(r->release, "\r\n") || strpbrk(r->etag, "\r\n"))
        return false;

    char tmp[400];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    fprintf(f, MAGIC "\nlink=%s\nurl=%s\nname=%s\nrelease=%s\netag=%s\ntotal=%llu\ndone=%llu\ncomplete=%d\n",
            r->link, r->url, r->name, r->release, r->etag, (unsigned long long)r->total,
            (unsigned long long)r->done, r->complete ? 1 : 0);
    if (fclose(f) != 0)
    {
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0)
    {
        remove(tmp);
        return false;
    }
    return true;
}

void dlresume_clear(const char *path)
{
    remove(path);
}

bool dlresume_matches(const dlresume_t *r, const char *link, const char *url,
                      const char *release, const char *name, uint64_t size_hint)
{
    if (strcmp(r->link, link) != 0 || strcmp(r->url, url) != 0 || strcmp(r->name, name) != 0 ||
        strcmp(r->release, release) != 0)
        return false;
    // La taille doit être la même quand on la connaît des deux côtés
    return size_hint == 0 || r->total == 0 || size_hint == r->total;
}
