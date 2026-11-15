// Réglages de TWiLight Menu++ (voir twlini.h)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "twlini.h"

#define INI_MAX (64 * 1024)   // Le vrai fichier fait quelques Ko

// Charge tout le fichier (chaîne terminée par un zéro). NULL s'il est illisible
// ou trop gros ; *missing dit s'il n'existe pas.
static char *load(const char *path, bool *missing)
{
    *missing = false;
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        *missing = true;
        return NULL;
    }
    char *buf = malloc(INI_MAX + 1);
    size_t n = buf ? fread(buf, 1, INI_MAX + 1, f) : 0;
    fclose(f);
    if (!buf || n > INI_MAX)
    {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

// Longueur de la ligne (sans le saut de ligne) et début de la suivante
static size_t line_len(const char *line, const char **next)
{
    size_t n = strcspn(line, "\n");
    *next = line[n] ? line + n + 1 : line + n;
    while (n > 0 && line[n - 1] == '\r')
        n--;
    return n;
}

// "[section]" ?
static bool is_section(const char *line, size_t len, const char *section)
{
    size_t n = strlen(section);
    return len >= n + 2 && line[0] == '[' && memcmp(line + 1, section, n) == 0 &&
           line[n + 1] == ']';
}

// Si la ligne est "clé = ...", renvoie la position de la valeur, sinon NULL
static const char *value_of(const char *line, size_t len, const char *key)
{
    const char *eq = memchr(line, '=', len);
    if (!eq)
        return NULL;
    size_t klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
        klen--;
    if (klen != strlen(key) || memcmp(line, key, klen) != 0)
        return NULL;
    const char *v = eq + 1;
    while (v < line + len && (*v == ' ' || *v == '\t'))
        v++;
    return v;
}

bool twlini_get(const char *path, const char *section, const char *key, char *out,
                size_t size)
{
    bool missing;
    char *buf = load(path, &missing);
    if (!buf)
        return false;

    bool inside = false, found = false;
    const char *line = buf, *next;
    for (; *line && !found; line = next)
    {
        size_t len = line_len(line, &next);
        if (len > 0 && line[0] == '[')
        {
            if (inside)
                break;   // Section suivante : la clé n'y est pas
            inside = is_section(line, len, section);
        }
        else if (inside)
        {
            const char *v = value_of(line, len, key);
            if (v)
            {
                size_t n = (size_t)(line + len - v);
                if (n >= size)
                    n = size - 1;
                memcpy(out, v, n);
                out[n] = '\0';
                found = true;
            }
        }
    }
    free(buf);
    return found;
}

bool twlini_set(const char *path, const char *section, const char *const *keys,
                const char *const *values, int n)
{
    if (n < 1 || n > 8)
        return false;

    bool missing;
    char *buf = load(path, &missing);
    if (!buf && !missing)
        return false;   // Illisible ou anormalement gros : on n'y touche pas

    char tmp[320];
    if (snprintf(tmp, sizeof(tmp), "%s.dsitmp", path) >= (int)sizeof(tmp))
    {
        free(buf);
        return false;
    }
    FILE *out = fopen(tmp, "wb");
    if (!out)
    {
        free(buf);
        return false;
    }

    bool done[8] = { false };
    bool inside = false, seen = false;
    int blanks = 0;   // Lignes vides de la section en attente (remises après les ajouts)

    // Écrit les clés pas encore rencontrées (à la fin de la section)
#define FLUSH_MISSING() do { \
        for (int i = 0; i < n; i++) \
            if (!done[i]) { fprintf(out, "%s = %s\r\n", keys[i], values[i]); done[i] = true; } \
    } while (0)

    const char *line = buf ? buf : "", *next;
    for (; *line; line = next)
    {
        size_t len = line_len(line, &next);
        if (len > 0 && line[0] == '[')
        {
            if (inside)
                FLUSH_MISSING();
            for (; blanks > 0; blanks--)
                fputs("\r\n", out);
            inside = !seen && is_section(line, len, section);
            seen = seen || inside;
        }
        else if (inside)
        {
            if (len == 0)
            {
                blanks++;
                continue;
            }
            for (; blanks > 0; blanks--)
                fputs("\r\n", out);
            int hit = -1;
            for (int i = 0; i < n && hit < 0; i++)
                if (value_of(line, len, keys[i]))
                    hit = i;
            if (hit >= 0)
            {
                if (!done[hit])
                    fprintf(out, "%s = %s\r\n", keys[hit], values[hit]);
                done[hit] = true;
                continue;
            }
        }
        fwrite(line, 1, len, out);
        fputs("\r\n", out);
    }
    if (!seen)
        fprintf(out, "[%s]\r\n", section);
    if (inside || !seen)
        FLUSH_MISSING();
    for (; blanks > 0; blanks--)
        fputs("\r\n", out);
#undef FLUSH_MISSING
    free(buf);

    if (fclose(out) != 0)
    {
        remove(tmp);
        return false;
    }
    // Le fichier complet est écrit : il remplace l'ancien
    remove(path);
    if (rename(tmp, path) != 0)
    {
        remove(tmp);
        return false;
    }
    return true;
}

bool twlini_autoboot_get(const char *root, const char *self)
{
    char path[300], val[280];
    snprintf(path, sizeof(path), "%s/" TWLINI_PATH, root);
    if (!self[0] || !twlini_get(path, "SRLOADER", "AUTORUNGAME", val, sizeof(val)) ||
        atoi(val) != 1)
        return false;
    return twlini_get(path, "SRLOADER", "ROM_PATH", val, sizeof(val)) &&
           strcasecmp(val, self) == 0;
}

bool twlini_autoboot_set(const char *root, const char *self, bool on)
{
    char path[300], val[280];
    snprintf(path, sizeof(path), "%s/" TWLINI_PATH, root);

    if (!on)
    {
        const char *const k[] = { "AUTORUNGAME" }, *const v[] = { "0" };
        return twlini_set(path, "SRLOADER", k, v, 1);
    }
    if (!self[0] || strpbrk(self, "\r\n"))
        return false;

    // Lancé depuis TWiLight Menu++ : il a déjà noté DSi Shop comme dernière ROM
    // avec sa propre façon de la lancer, on n'y touche pas
    if (twlini_get(path, "SRLOADER", "ROM_PATH", val, sizeof(val)) &&
        strcasecmp(val, self) == 0)
    {
        const char *const k[] = { "AUTORUNGAME" }, *const v[] = { "1" };
        return twlini_set(path, "SRLOADER", k, v, 1);
    }

    // Sinon : DSi Shop devient la dernière ROM (homebrew sur la carte SD de la
    // console, lancé directement)
    const char *const k[] = { "AUTORUNGAME", "ROM_PATH", "LAUNCH_TYPE",
                              "PREVIOUS_USED_DEVICE", "SLOT1_LAUNCHED" };
    const char *const v[] = { "1", self, "2", "0", "0" };
    return twlini_set(path, "SRLOADER", k, v, 5);
}

// ---------------------------------------------------------------------------
// Lancer un jeu
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// CRC16 de l'en-tête d'une ROM DS (init 0xFFFF, poly 0xA001)
static uint16_t header_crc16(const uint8_t *data, size_t len)
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

static bool read_words(FILE *f, uint32_t pos, uint32_t *out, int n)
{
    uint8_t b[16];
    if (n > 4 || fseek(f, (long)pos, SEEK_SET) != 0 || fread(b, 4, (size_t)n, f) != (size_t)n)
        return false;
    for (int i = 0; i < n; i++)
        out[i] = rd32(b + i * 4);
    return true;
}

// Mêmes règles que le menu de TWiLight Menu++ (iconTitle.cpp) : un fichier est un
// jeu du commerce si le début de son code ARM9 est celui du SDK de Nintendo, sinon
// c'est un homebrew ; un homebrew est "moderne" s'il sait lire la SD sans pilote DLDI.
rom_kind_t twlini_classify_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return ROM_INVALID;
    uint8_t h[0x200];
    size_t n = fread(h, 1, sizeof(h), f);
    if (n != sizeof(h) || header_crc16(h, 0x15E) != rd16(h + 0x15E))
    {
        fclose(f);
        return ROM_INVALID;
    }

    uint32_t a9off = rd32(h + 0x20), a9exec = rd32(h + 0x24), a9dst = rd32(h + 0x28);
    uint32_t a7off = rd32(h + 0x30), a7exec = rd32(h + 0x34), a7dst = rd32(h + 0x38);
    uint32_t a7size = rd32(h + 0x3C), access = rd32(h + 0x1B4);
    uint8_t unit = h[0x12];
    if (!(a9dst >= 0x02000000 && a9dst < 0x03000000 && a9exec >= 0x02000000 &&
          a9exec < 0x03000000))
    {
        fclose(f);
        return ROM_INVALID;
    }

    uint32_t sig[4] = { 0 };
    uint32_t start = memcmp(h + 0x0C, "BIG", 3) == 0 ? 0x02000800 : a9exec;
    if (!read_words(f, a9off + start - a9dst, sig, 4))
    {
        fclose(f);
        return ROM_INVALID;
    }

    bool homebrew = true;
    if ((sig[0] == 0xE3A0C301 || (sig[0] >= 0xEA000000 && sig[0] < 0xEC000000)) &&
        sig[1] == 0xE58CC208)
    {
        if ((sig[2] >= 0xEB000000 && sig[2] < 0xEC000000) &&
            (sig[3] >= 0xE3A00000 && sig[3] < 0xE3A01000))
            homebrew = false;                                   // SDK 2 et TWL SDK 5
        else if (sig[2] == 0xE1DC00B6 && sig[3] == 0xE3500000)
            homebrew = false;                                   // SDK 3 à 5
        else if (sig[2] == 0xEAFFFFFF && sig[3] == 0xE1DC00B6)
            homebrew = false;                                   // SDK 4
    }
    else if (memcmp(h + 0x0C, "HNA", 3) == 0)
        homebrew = false;                                       // Modcrypt

    if (!homebrew)
    {
        fclose(f);
        return (unit != 0 && (access & 0x10)) ? ROM_DSIWARE : ROM_COMMERCIAL;
    }

    bool modern = true;
    if (sig[0] == 0xE3A00301 && sig[1] == 0xE5800208 && sig[2] == 0xE3A00013 &&
        sig[3] == 0xE129F000)
    {
        if (a7exec >= 0x037F0000 && a7dst >= 0x037F0000)
        {
            // Code ARM7 chargé en IWRAM : moderne seulement s'il touche au registre
            // de la SD de la DSi (0x04004800), sinon il lui faut un pilote DLDI
            modern = false;
            uint32_t at = 0;
            if (read_words(f, a7off + 0xC8, &at, 1))
            {
                if (at >= 0x037F0000 && at < 0x03810000)
                {
                    at = (at + 3) & ~3u;
                    uint32_t pos = at - a7dst + a7off + 0x200, end = a7off + a7size;
                    for (int i = 0; i < 0x800; i += 4)
                    {
                        uint32_t w;
                        if (!read_words(f, pos, &w, 1))
                            break;
                        if (w == 0x04004800)
                        {
                            modern = true;
                            break;
                        }
                        pos += 4;
                        if (pos >= end)
                            break;
                    }
                }
                else
                    modern = true;
            }
        }
    }
    else if (unit == 0 && (memcmp(h, "NMP4BOOT", 8) == 0 ||
                           (a7exec >= 0x037F0000 && a7dst >= 0x037F0000)))
        modern = false;

    fclose(f);
    return modern ? ROM_HOMEBREW_MODERN : ROM_HOMEBREW_OLD;
}

#define PREV_AUTORUN "_dsishop/prev_autorun"

twl_launch_err_t twlini_launch_set(const char *root, const char *rom_path, rom_kind_t *kind_out)
{
    size_t r = strlen(root);
    if (strncmp(rom_path, root, r) != 0 || rom_path[r] != '/' || strpbrk(rom_path, "\r\n"))
        return LAUNCH_NOT_SD;

    rom_kind_t kind = twlini_classify_rom(rom_path);
    if (kind_out)
        *kind_out = kind;
    if (kind == ROM_INVALID)
        return LAUNCH_BAD_ROM;
    if (kind == ROM_DSIWARE)
        return LAUNCH_DSIWARE;

    char ini[300], prev_path[300], val[16];
    snprintf(ini, sizeof(ini), "%s/" TWLINI_PATH, root);
    snprintf(prev_path, sizeof(prev_path), "%s/" PREV_AUTORUN, root);

    // Note l'ancien réglage, sans écraser une note plus ancienne (lancements enchaînés)
    bool noted = false;
    struct stat st;
    if (stat(prev_path, &st) != 0)
    {
        char dir[300];
        snprintf(dir, sizeof(dir), "%s/_dsishop", root);
        mkdir(dir, 0777);
        bool was = twlini_get(ini, "SRLOADER", "AUTORUNGAME", val, sizeof(val)) && atoi(val) == 1;
        FILE *f = fopen(prev_path, "wb");
        if (!f)
            return LAUNCH_WRITE;
        fputs(was ? "1" : "0", f);
        if (fclose(f) != 0)
        {
            remove(prev_path);
            return LAUNCH_WRITE;
        }
        noted = true;
    }

    // Jeu du commerce et homebrew ancien : nds-bootstrap (LAUNCH_TYPE 1) ;
    // homebrew moderne : lancement direct (LAUNCH_TYPE 2), comme le fait TWiLight
    const char *const keys[] = { "AUTORUNGAME", "ROM_PATH", "LAUNCH_TYPE", "HOMEBREW_BOOTSTRAP",
                                 "PREVIOUS_USED_DEVICE", "SLOT1_LAUNCHED" };
    const char *const values[] = { "1", rom_path, kind == ROM_HOMEBREW_MODERN ? "2" : "1",
                                   kind == ROM_COMMERCIAL ? "0" : "1", "0", "0" };
    if (!twlini_set(ini, "SRLOADER", keys, values, 6))
    {
        if (noted)
            remove(prev_path);
        return LAUNCH_WRITE;
    }
    return LAUNCH_OK;
}

bool twlini_launch_restore(const char *root)
{
    char ini[300], prev_path[300], val[16] = "";
    snprintf(ini, sizeof(ini), "%s/" TWLINI_PATH, root);
    snprintf(prev_path, sizeof(prev_path), "%s/" PREV_AUTORUN, root);

    FILE *f = fopen(prev_path, "rb");
    if (!f)
        return false;
    size_t n = fread(val, 1, sizeof(val) - 1, f);
    fclose(f);
    val[n] = '\0';

    const char *const keys[] = { "AUTORUNGAME" };
    const char *const values[] = { val[0] == '1' ? "1" : "0" };
    if (!twlini_set(ini, "SRLOADER", keys, values, 1))
        return false;
    remove(prev_path);
    return true;
}
