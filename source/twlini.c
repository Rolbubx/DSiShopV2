// Réglages de TWiLight Menu++ (voir twlini.h)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
