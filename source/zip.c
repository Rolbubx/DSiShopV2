// Extraction ZIP / ZIP64 (méthodes "stored" et "deflate")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <zlib.h>

#include "zip.h"

#define SIG_LOCAL     0x04034b50u
#define SIG_CENTRAL   0x02014b50u
#define SIG_EOCD      0x06054b50u
#define SIG_EOCD64    0x06064b50u
#define SIG_LOCATOR64 0x07064b50u

#define MAX_CD_SIZE   (8u * 1024 * 1024) // Répertoire central max chargé en RAM
#define FAT32_MAX     0xFFFFFFFFull      // Taille max d'un fichier FAT32 (exclue)

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool read_at(sfile_t *sf, uint64_t pos, void *buf, size_t len)
{
    return sfile_seek(sf, pos) && sfile_read(sf, buf, len) == len;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return lx <= ls && strcasecmp(s + ls - lx, suffix) == 0;
}

// Infos d'une entrée, avec les champs ZIP64 résolus
typedef struct {
    uint16_t method;
    uint32_t crc;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t local_offset;
} entry_t;

static void apply_zip64_extra(entry_t *e, const uint8_t *extra, size_t len,
                              bool need_uncomp, bool need_comp, bool need_off)
{
    size_t i = 0;
    while (i + 4 <= len)
    {
        uint16_t id = rd16(extra + i), sz = rd16(extra + i + 2);
        const uint8_t *d = extra + i + 4;
        if (i + 4 + sz > len)
            break;
        if (id == 0x0001)
        {
            // Les champs n'apparaissent que s'ils valent 0xFFFFFFFF, dans cet ordre
            size_t o = 0;
            if (need_uncomp && o + 8 <= sz) { e->uncomp_size = rd64(d + o); o += 8; }
            if (need_comp && o + 8 <= sz)   { e->comp_size = rd64(d + o); o += 8; }
            if (need_off && o + 8 <= sz)    { e->local_offset = rd64(d + o); o += 8; }
            return;
        }
        i += 4 + sz;
    }
}

// Trouve le répertoire central (ZIP classique ou ZIP64)
static zip_err_t find_central_dir(sfile_t *sf, uint64_t *cd_offset,
                                  uint64_t *cd_size, uint64_t *entries)
{
    uint64_t size = sf->size;
    if (size < 22)
        return ZIP_ERR_FORMAT;

    // L'EOCD est dans les 64 Ko + 22 derniers octets (commentaire max 64 Ko)
    size_t tail = size < 65557 ? (size_t)size : 65557;
    uint8_t *buf = malloc(tail);
    if (!buf)
        return ZIP_ERR_MEMORY;
    if (!read_at(sf, size - tail, buf, tail))
    {
        free(buf);
        return ZIP_ERR_CORRUPT;
    }

    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
    {
        if (rd32(buf + i) == SIG_EOCD)
        {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
    {
        free(buf);
        return ZIP_ERR_FORMAT;
    }

    uint64_t eocd_pos = size - tail + (uint64_t)eocd;
    *entries = rd16(buf + eocd + 10);
    *cd_size = rd32(buf + eocd + 12);
    *cd_offset = rd32(buf + eocd + 16);
    free(buf);

    // ZIP64 : un "locator" de 20 octets précède l'EOCD
    if (*entries == 0xFFFF || *cd_size == 0xFFFFFFFF || *cd_offset == 0xFFFFFFFF)
    {
        uint8_t loc[20], rec[56];
        if (eocd_pos < 20 || !read_at(sf, eocd_pos - 20, loc, 20) ||
            rd32(loc) != SIG_LOCATOR64)
            return ZIP_ERR_FORMAT;

        uint64_t rec_pos = rd64(loc + 8);
        if (!read_at(sf, rec_pos, rec, 56) || rd32(rec) != SIG_EOCD64)
            return ZIP_ERR_FORMAT;

        *entries = rd64(rec + 32);
        *cd_size = rd64(rec + 40);
        *cd_offset = rd64(rec + 48);
    }

    if (*cd_offset + *cd_size > size)
        return ZIP_ERR_FORMAT;
    return ZIP_OK;
}

// Décompresse une entrée dans out_path (vérifie taille et CRC). La progression
// annoncée est base + octets écrits, sur un total de `total`.
static zip_err_t extract_entry(sfile_t *sf, const entry_t *e, const char *out_path,
                               zip_progress_t progress, void *userdata,
                               uint64_t base, uint64_t total)
{
    zip_err_t err;

    if (e->method != 0 && e->method != 8)
        return ZIP_ERR_METHOD;
    if (e->uncomp_size >= FAT32_MAX)
        return ZIP_ERR_TOO_BIG;

    // En-tête local : sa taille de champ "extra" peut différer du central
    uint8_t lh[30];
    if (!read_at(sf, e->local_offset, lh, 30) || rd32(lh) != SIG_LOCAL)
        return ZIP_ERR_CORRUPT;
    uint64_t data_pos = e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data_pos + e->comp_size > sf->size)
        return ZIP_ERR_CORRUPT;
    sfile_seek(sf, data_pos);

    FILE *out = fopen(out_path, "wb");
    if (!out)
        return ZIP_ERR_WRITE;
    setvbuf(out, NULL, _IOFBF, 32 * 1024);

    const size_t CHUNK = 16 * 1024;
    uint8_t *in_buf = malloc(CHUNK), *out_buf = malloc(CHUNK);
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    err = (in_buf && out_buf) ? ZIP_OK : ZIP_ERR_MEMORY;
    bool z_init = false;
    if (err == ZIP_OK && e->method == 8)
    {
        if (inflateInit2(&zs, -MAX_WBITS) == Z_OK)
            z_init = true;
        else
            err = ZIP_ERR_MEMORY;
    }

    uint64_t remaining = e->comp_size, written = 0;
    uLong crc = crc32(0, Z_NULL, 0);
    int zret = Z_OK;

    while (err == ZIP_OK && remaining > 0 && zret != Z_STREAM_END)
    {
        size_t want = remaining < CHUNK ? (size_t)remaining : CHUNK;
        size_t got = sfile_read(sf, in_buf, want);
        if (got != want)
        {
            err = ZIP_ERR_CORRUPT;
            break;
        }
        remaining -= got;

        if (e->method == 0)
        {
            crc = crc32(crc, in_buf, (uInt)got);
            if (fwrite(in_buf, 1, got, out) != got)
                err = ZIP_ERR_WRITE;
            written += got;
        }
        else
        {
            zs.next_in = in_buf;
            zs.avail_in = (uInt)got;
            do
            {
                zs.next_out = out_buf;
                zs.avail_out = (uInt)CHUNK;
                zret = inflate(&zs, Z_NO_FLUSH);
                if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR)
                {
                    err = ZIP_ERR_CORRUPT;
                    break;
                }
                size_t have = CHUNK - zs.avail_out;
                crc = crc32(crc, out_buf, (uInt)have);
                if (fwrite(out_buf, 1, have, out) != have)
                    err = ZIP_ERR_WRITE;
                written += have;
                if (written > e->uncomp_size)
                    err = ZIP_ERR_CORRUPT;
            } while (err == ZIP_OK && zs.avail_out == 0 && zret != Z_STREAM_END);
        }

        if (err == ZIP_OK && progress &&
            progress(userdata, base + written, total) != 0)
            err = ZIP_ERR_CANCELLED;
    }

    if (z_init)
        inflateEnd(&zs);
    free(in_buf);
    free(out_buf);
    if (fclose(out) != 0 && err == ZIP_OK)
        err = ZIP_ERR_WRITE;

    if (err == ZIP_OK && (written != e->uncomp_size || (uint32_t)crc != e->crc))
        err = ZIP_ERR_CORRUPT;

    if (err != ZIP_OK)
        remove(out_path);
    return err;
}

zip_err_t zip_extract_rom(sfile_t *sf, const char *out_path,
                          char *name_out, size_t name_size,
                          zip_progress_t progress, void *userdata)
{
    uint64_t cd_offset, cd_size, entries;
    zip_err_t err = find_central_dir(sf, &cd_offset, &cd_size, &entries);
    if (err != ZIP_OK)
        return err;

    if (cd_size > MAX_CD_SIZE)
        return ZIP_ERR_MEMORY;

    uint8_t *cd = malloc(cd_size ? (size_t)cd_size : 1);
    if (!cd)
        return ZIP_ERR_MEMORY;
    if (!read_at(sf, cd_offset, cd, (size_t)cd_size))
    {
        free(cd);
        return ZIP_ERR_CORRUPT;
    }

    // Parcourt le répertoire central : .nds en priorité, sinon .dsi
    entry_t best = { 0 };
    int best_rank = 99;
    size_t off = 0;

    for (uint64_t i = 0; i < entries && off + 46 <= cd_size; i++)
    {
        const uint8_t *h = cd + off;
        if (rd32(h) != SIG_CENTRAL)
            break;

        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (off + 46 + nlen + xlen + clen > cd_size)
            break;

        char name[256];
        size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, h + 46, n);
        name[n] = '\0';

        int rank = 99;
        if (strstr(name, "__MACOSX") == NULL && name[n ? n - 1 : 0] != '/')
        {
            if (ends_with_ci(name, ".nds"))
                rank = 0;
            else if (ends_with_ci(name, ".dsi"))
                rank = 1;
        }

        if (rank < best_rank)
        {
            entry_t e = {
                .method = rd16(h + 10),
                .crc = rd32(h + 16),
                .comp_size = rd32(h + 20),
                .uncomp_size = rd32(h + 24),
                .local_offset = rd32(h + 42),
            };
            apply_zip64_extra(&e, h + 46 + nlen, xlen,
                              e.uncomp_size == 0xFFFFFFFF,
                              e.comp_size == 0xFFFFFFFF,
                              e.local_offset == 0xFFFFFFFF);
            best = e;
            best_rank = rank;

            // Nom sans dossier, avec caractères interdits par FAT remplacés
            const char *base = strrchr(name, '/');
            base = base ? base + 1 : name;
            snprintf(name_out, name_size, "%s", base);
            for (char *p = name_out; *p; p++)
                if ((unsigned char)*p < 0x20 || strchr("\\:*?\"<>|", *p))
                    *p = '_';
        }

        off += 46 + nlen + xlen + clen;
    }
    free(cd);

    if (best_rank == 99)
        return ZIP_ERR_NO_ROM;
    return extract_entry(sf, &best, out_path, progress, userdata, 0, best.uncomp_size);
}

// ---------------------------------------------------------------------------
// Extraction de toute une archive (mise à jour du système)
// ---------------------------------------------------------------------------

// Un chemin d'archive acceptable : relatif, sans "..", sans caractère interdit
// par FAT. Tout le reste fait refuser l'archive entière.
static bool safe_path(const char *name)
{
    if (!name[0] || name[0] == '/')
        return false;
    for (const char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\:*?\"<>|", *p))
            return false;
    // Aucun élément vide, "." ou ".."
    const char *p = name;
    while (*p)
    {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        p += n;
        if (*p == '/')
            p++;
    }
    return true;
}

// Crée les dossiers parents de path (à partir de l'octet `from`)
static void make_parents(char *path, size_t from)
{
    for (char *p = path + from; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(path, 0777);
            *p = '/';
        }
    }
}

// Lit l'entrée suivante du répertoire central. Renvoie false à la fin (ou si
// le répertoire est abîmé : *bad est alors mis à true).
static bool next_entry(const uint8_t *cd, uint64_t cd_size, size_t *off, entry_t *e,
                       char *name, size_t name_size, bool *bad)
{
    if (*off + 46 > cd_size || rd32(cd + *off) != SIG_CENTRAL)
    {
        *bad = *off != cd_size;
        return false;
    }
    const uint8_t *h = cd + *off;
    uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
    if (*off + 46 + nlen + xlen + clen > cd_size || nlen >= name_size)
    {
        *bad = true;
        return false;
    }
    memcpy(name, h + 46, nlen);
    name[nlen] = '\0';

    e->method = rd16(h + 10);
    e->crc = rd32(h + 16);
    e->comp_size = rd32(h + 20);
    e->uncomp_size = rd32(h + 24);
    e->local_offset = rd32(h + 42);
    apply_zip64_extra(e, h + 46 + nlen, xlen, e->uncomp_size == 0xFFFFFFFF,
                      e->comp_size == 0xFFFFFFFF, e->local_offset == 0xFFFFFFFF);
    *off += 46 + nlen + xlen + clen;
    return true;
}

zip_err_t zip_extract_all(sfile_t *sf, const char *dest_dir,
                          zip_progress_t progress, void *userdata)
{
    uint64_t cd_offset, cd_size, entries;
    zip_err_t err = find_central_dir(sf, &cd_offset, &cd_size, &entries);
    if (err != ZIP_OK)
        return err;
    if (cd_size > MAX_CD_SIZE)
        return ZIP_ERR_MEMORY;

    uint8_t *cd = malloc(cd_size ? (size_t)cd_size : 1);
    if (!cd)
        return ZIP_ERR_MEMORY;
    if (!read_at(sf, cd_offset, cd, (size_t)cd_size))
    {
        free(cd);
        return ZIP_ERR_CORRUPT;
    }

    char name[ZIP_PATH_MAX], path[ZIP_PATH_MAX + 256], tmp[ZIP_PATH_MAX + 264];
    size_t root_len = strlen(dest_dir) + 1;
    entry_t e;
    bool bad = false, method_bad = false;

    // 1re passe : on vérifie tous les chemins avant d'écrire quoi que ce soit
    uint64_t total = 0, count = 0;
    size_t off = 0;
    while (next_entry(cd, cd_size, &off, &e, name, sizeof(name), &bad))
    {
        if (e.method != 0 && e.method != 8)
            method_bad = true;
        if (!safe_path(name))
            bad = true;
        if (bad || method_bad)
            break;
        total += e.uncomp_size;
        count++;
    }
    if (bad || method_bad || count != entries || strlen(dest_dir) >= 250)
    {
        free(cd);
        return method_bad && !bad ? ZIP_ERR_METHOD : ZIP_ERR_FORMAT;
    }

    // 2e passe : chaque fichier est écrit à côté de sa destination puis la
    // remplace, pour qu'une coupure ne laisse jamais un fichier à moitié écrit
    uint64_t done = 0;
    off = 0;
    while (err == ZIP_OK && next_entry(cd, cd_size, &off, &e, name, sizeof(name), &bad))
    {
        snprintf(path, sizeof(path), "%s/%s", dest_dir, name);
        make_parents(path, root_len);

        size_t n = strlen(path);
        if (path[n - 1] == '/')
        {
            path[n - 1] = '\0';
            mkdir(path, 0777);
            continue;
        }

        snprintf(tmp, sizeof(tmp), "%s.dsitmp", path);
        err = extract_entry(sf, &e, tmp, progress, userdata, done, total);
        if (err != ZIP_OK)
            break;
        remove(path);
        if (rename(tmp, path) != 0)
        {
            remove(tmp);
            err = ZIP_ERR_WRITE;
        }
        done += e.uncomp_size;
    }
    free(cd);
    return err;
}

const char *zip_strerror(zip_err_t err)
{
    switch (err)
    {
        case ZIP_OK:            return "OK";
        case ZIP_ERR_FORMAT:    return "Archive .zip invalide";
        case ZIP_ERR_NO_ROM:    return "Aucun fichier .nds ou .dsi dans l'archive";
        case ZIP_ERR_METHOD:    return "Méthode de compression .zip non supportée";
        case ZIP_ERR_TOO_BIG:   return "La ROM dépasse 4 Go : impossible sur une carte SD en FAT32";
        case ZIP_ERR_CORRUPT:   return "Archive .zip corrompue (CRC invalide)";
        case ZIP_ERR_WRITE:     return "Impossible d'écrire sur la carte SD (pleine ?)";
        case ZIP_ERR_MEMORY:    return "Mémoire insuffisante pour lire l'archive";
        case ZIP_ERR_CANCELLED: return "Installation annulée";
    }
    return "Erreur inconnue";
}
