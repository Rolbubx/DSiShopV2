// Fichier découpé en morceaux (voir splitfile.h)

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "splitfile.h"

void sfile_part_path(const char *base, unsigned int n, char *out, size_t size)
{
    snprintf(out, size, "%s.%03u", base, n);
}

static bool open_part(sfile_t *sf, unsigned int n)
{
    if (sf->f && sf->part == n)
        return true;

    if (n >= SPLIT_MAX_PARTS)
    {
        sf->error = true;
        return false;
    }

    if (sf->f)
    {
        if (fclose(sf->f) != 0)
            sf->error = true;
        sf->f = NULL;
    }

    char path[256];
    sfile_part_path(sf->base, n, path, sizeof(path));
    sf->f = fopen(path, sf->writing ? "wb" : "rb");
    if (!sf->f)
    {
        sf->error = true;
        return false;
    }
    if (sf->writing)
        setvbuf(sf->f, NULL, _IOFBF, 32 * 1024);
    sf->part = n;
    return true;
}

bool sfile_open_write(sfile_t *sf, const char *base, uint64_t chunk)
{
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->base, sizeof(sf->base), "%s", base);
    sf->chunk = chunk;
    sf->writing = true;
    sfile_remove(base); // Pas de vieux morceaux qui traînent
    return open_part(sf, 0);
}

size_t sfile_write(sfile_t *sf, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t done = 0;

    while (done < len && !sf->error)
    {
        unsigned int n = (unsigned int)(sf->pos / sf->chunk);
        uint64_t room = sf->chunk - (sf->pos % sf->chunk);
        size_t want = len - done;
        if (want > room)
            want = (size_t)room;

        if (!open_part(sf, n))
            break;

        size_t w = fwrite(p + done, 1, want, sf->f);
        done += w;
        sf->pos += w;
        if (sf->pos > sf->size)
            sf->size = sf->pos;
        if (w != want)
            sf->error = true;
    }

    return done;
}

bool sfile_open_read(sfile_t *sf, const char *base, uint64_t chunk)
{
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->base, sizeof(sf->base), "%s", base);
    sf->chunk = chunk;

    // Taille totale = somme des morceaux consécutifs
    for (unsigned int n = 0; n < SPLIT_MAX_PARTS; n++)
    {
        char path[256];
        struct stat st;
        sfile_part_path(base, n, path, sizeof(path));
        if (stat(path, &st) != 0)
        {
            if (n == 0)
                return false;
            break;
        }
        sf->size += (uint64_t)st.st_size;
    }

    return open_part(sf, 0);
}

bool sfile_seek(sfile_t *sf, uint64_t pos)
{
    if (pos > sf->size)
        return false;
    sf->pos = pos;
    return true;
}

size_t sfile_read(sfile_t *sf, void *data, size_t len)
{
    uint8_t *p = data;
    size_t done = 0;

    while (done < len && sf->pos < sf->size)
    {
        unsigned int n = (unsigned int)(sf->pos / sf->chunk);
        uint64_t off = sf->pos % sf->chunk;
        uint64_t room = sf->chunk - off;
        size_t want = len - done;
        if (want > room)
            want = (size_t)room;
        if (want > sf->size - sf->pos)
            want = (size_t)(sf->size - sf->pos);

        if (!open_part(sf, n))
            break;
        // off < chunk <= 1 Gio : tient dans un long
        if (fseek(sf->f, (long)off, SEEK_SET) != 0)
        {
            sf->error = true;
            break;
        }

        size_t r = fread(p + done, 1, want, sf->f);
        done += r;
        sf->pos += r;
        if (r != want)
        {
            sf->error = true;
            break;
        }
    }

    return done;
}

bool sfile_close(sfile_t *sf)
{
    if (sf->f)
    {
        if (fclose(sf->f) != 0)
            sf->error = true;
        sf->f = NULL;
    }
    return !sf->error;
}

unsigned int sfile_part_count(const sfile_t *sf)
{
    if (sf->size == 0)
        return 1;
    return (unsigned int)((sf->size + sf->chunk - 1) / sf->chunk);
}

void sfile_remove(const char *base)
{
    char path[256];
    for (unsigned int n = 0; n < SPLIT_MAX_PARTS; n++)
    {
        sfile_part_path(base, n, path, sizeof(path));
        if (remove(path) != 0)
            break;
    }
}
