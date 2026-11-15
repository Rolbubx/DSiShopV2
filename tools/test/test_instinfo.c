// Tests de instinfo.c sur PC.

#include <stdio.h>
#include <string.h>

#include "../../source/instinfo.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC : " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

// Position juste après l'adresse du n-ième fichier (comme json_string_after)
static const char *after_url(const char *json, int n)
{
    const char *p = json;
    for (int i = 0; i <= n; i++)
    {
        p = strstr(p, "\"browser_download_url\"");
        p = strchr(p + 22, '"');          // ouvrant
        p = strchr(p + 1, '"') + 1;       // fermant
    }
    return p;
}

int main(void)
{
    printf("instinfo : taille d'un fichier de release\n");
    // Réponse de notre serveur (ordre : name, size, browser_download_url)
    const char *ours =
        "{\"tag_name\": \"v1.2\", \"name\": \"Pong\", \"assets\": ["
        "{\"name\": \"a.nds\", \"size\": 131072, \"browser_download_url\": \"https://x/a.nds\"}, "
        "{\"name\": \"b.zip\", \"size\": 4294967296, \"browser_download_url\": \"https://x/b.zip\"}, "
        "{\"name\": \"c.txt\", \"browser_download_url\": \"https://x/c.txt\"}]}";
    CHECK(instinfo_asset_size(ours, after_url(ours, 0)) == 131072, "1er fichier");
    CHECK(instinfo_asset_size(ours, after_url(ours, 1)) == 4294967296ull, "2e fichier (> 4 Go)");
    CHECK(instinfo_asset_size(ours, after_url(ours, 2)) == 0, "fichier sans taille : %llu",
          (unsigned long long)instinfo_asset_size(ours, after_url(ours, 2)));
    // Mise en forme différente (retours à la ligne, pas d'espace)
    const char *pretty = "{\"assets\":[{\n  \"name\":\"d.nds\",\n  \"size\" :  99,\n"
                         "  \"browser_download_url\":\"https://x/d.nds\"}]}";
    CHECK(instinfo_asset_size(pretty, after_url(pretty, 0)) == 99, "JSON mis en forme");
    CHECK(instinfo_asset_size("", "") == 0, "JSON vide");

    printf("instinfo : projet depuis l'adresse de l'API\n");
    char o[40], r[64];
    CHECK(instinfo_repo_of_api_url("https://dsi-shop.duckdns.org/api/repos/auteur/pong/releases/latest",
                                   o, sizeof(o), r, sizeof(r)) &&
          !strcmp(o, "auteur") && !strcmp(r, "pong"), "latest");
    CHECK(instinfo_repo_of_api_url("https://h/api/repos/a-b/c.d_e/releases/tags/v1", o, sizeof(o),
                                   r, sizeof(r)) && !strcmp(o, "a-b") && !strcmp(r, "c.d_e"), "tags");
    CHECK(!instinfo_repo_of_api_url("https://h/autre", o, sizeof(o), r, sizeof(r)), "sans /repos/");
    CHECK(!instinfo_repo_of_api_url("https://h/repos/seul", o, sizeof(o), r, sizeof(r)), "sans projet");
    CHECK(!instinfo_repo_of_api_url("https://h/repos/aaaaaaaaaaaaaaaa/x", o, 8, r, sizeof(r)),
          "auteur trop long");

    printf("instinfo : hôte d'une adresse\n");
    char h[32];
    static const struct { const char *url, *host; } hosts[] = {
        { "https://exemple.fr/dossier/jeu.nds", "exemple.fr" },
        { "http://exemple.fr:8080/x?y=1", "exemple.fr" },
        { "https://user:pass@site.org/a", "site.org" },
        { "site.org/jeu.nds", "site.org" },
        { "https://tres-long-nom-de-domaine-vraiment.example.com/x", "tres-long-nom-de-domaine-vraime" },
    };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
    {
        instinfo_url_host(hosts[i].url, h, sizeof(h));
        CHECK(strcmp(h, hosts[i].host) == 0, "%s -> %s", hosts[i].url, h);
    }

    printf("instinfo : débit et durée restante\n");
    char t[32];
    instinfo_format_speed(t, sizeof(t), 0);
    CHECK(t[0] == '\0', "débit nul");
    instinfo_format_speed(t, sizeof(t), 52 * 1024);
    CHECK(!strcmp(t, "52 Ko/s"), "%s", t);
    instinfo_format_speed(t, sizeof(t), 1024 * 1024 * 13 / 10);
    CHECK(!strcmp(t, "1,2 Mo/s") || !strcmp(t, "1,3 Mo/s"), "%s", t);
    instinfo_format_eta(t, sizeof(t), 1000, 0);
    CHECK(t[0] == '\0', "durée sans débit");
    instinfo_format_eta(t, sizeof(t), 100, 10);
    CHECK(!strcmp(t, "~ 10 s"), "%s", t);
    instinfo_format_eta(t, sizeof(t), 130 * 10, 10);
    CHECK(!strcmp(t, "~ 2 min 10 s"), "%s", t);
    instinfo_format_eta(t, sizeof(t), 3900ull * 10, 10);
    CHECK(!strcmp(t, "~ 1 h 05"), "%s", t);
    instinfo_format_eta(t, sizeof(t), 1, 1000);
    CHECK(!strcmp(t, "~ 1 s"), "arrondi au-dessus : %s", t);

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
