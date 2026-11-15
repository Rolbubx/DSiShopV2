// Pré-description d'une installation (voir instinfo.h)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "instinfo.h"

uint64_t instinfo_asset_size(const char *json, const char *end)
{
    // Le "size" du fichier précède son "browser_download_url" : on cherche le
    // dernier "size" juste avant, sans remonter dans le fichier précédent
    static const char KEY[] = "\"size\"";
    static const char URL[] = "\"browser_download_url\"";
    const char *lo = (end - json > 1000) ? end - 1000 : json;

    // Début de l'objet de ce fichier : juste après l'adresse du précédent
    const char *p = lo, *obj = lo;
    while ((p = strstr(p, URL)) != NULL && p < end)
    {
        const char *close = strchr(p + sizeof(URL), '}');
        if (close && close < end)
            obj = close;
        p += sizeof(URL) - 1;
    }

    const char *best = NULL;
    for (p = obj; p + sizeof(KEY) - 1 <= end; p++)
        if (memcmp(p, KEY, sizeof(KEY) - 1) == 0)
            best = p;
    if (!best)
        return 0;

    p = best + sizeof(KEY) - 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        p++;
    if (!isdigit((unsigned char)*p))
        return 0;
    return strtoull(p, NULL, 10);
}

bool instinfo_repo_of_api_url(const char *api_url, char *owner, size_t owner_size,
                              char *repo, size_t repo_size)
{
    const char *p = strstr(api_url, "/repos/");
    if (!p)
        return false;
    p += 7;
    size_t n = strcspn(p, "/");
    if (n == 0 || n >= owner_size || p[n] != '/')
        return false;
    memcpy(owner, p, n);
    owner[n] = '\0';
    p += n + 1;
    n = strcspn(p, "/?#");
    if (n == 0 || n >= repo_size)
        return false;
    memcpy(repo, p, n);
    repo[n] = '\0';
    return true;
}

void instinfo_url_host(const char *url, char *out, size_t size)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = memchr(p, '@', strcspn(p, "/"));   // identifiant éventuel
    if (at)
        p = at + 1;
    size_t n = strcspn(p, "/:?#");
    if (n >= size)
        n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

void instinfo_format_speed(char *out, size_t size, uint64_t bps)
{
    const uint64_t KB = 1024, MB = KB * 1024;
    if (bps == 0)
        out[0] = '\0';
    else if (bps >= MB)
        snprintf(out, size, "%u,%u Mo/s", (unsigned)(bps / MB),
                 (unsigned)((bps % MB) * 10 / MB));
    else
        snprintf(out, size, "%u Ko/s", (unsigned)((bps + KB - 1) / KB));
}

void instinfo_format_eta(char *out, size_t size, uint64_t remaining, uint64_t bps)
{
    if (bps == 0)
    {
        out[0] = '\0';
        return;
    }
    uint64_t s = (remaining + bps - 1) / bps;
    if (s < 60)
        snprintf(out, size, "~ %u s", (unsigned)s);
    else if (s < 3600)
        snprintf(out, size, "~ %u min %02u s", (unsigned)(s / 60), (unsigned)(s % 60));
    else
        snprintf(out, size, "~ %u h %02u", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}
