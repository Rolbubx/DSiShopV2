// Tests de sdscan.c sur PC :  ./test_sdscan <dossier de travail>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../source/sdscan.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC : " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static char root[400];

static void put(const char *rel, size_t size)
{
    char path[600], dir[600];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + strlen(root) + 1; *p; p++)
        if (*p == '/')
        {
            *p = '\0';
            mkdir(dir, 0777);
            *p = '/';
        }
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; f && i < size; i++)
        fputc('x', f);
    if (f)
        fclose(f);
}

static sdscan_t scan;

static bool has(const char *rel)
{
    char want[600];
    snprintf(want, sizeof(want), "%s/%s", root, rel);
    for (int i = 0; i < scan.count; i++)
        if (strcmp(scan.games[i].path, want) == 0)
            return true;
    return false;
}

static void run(int budget)
{
    sdscan_start(&scan, root);
    int steps = 0;
    while (!sdscan_step(&scan, budget))
        steps++;
    CHECK(steps < 100000, "boucle sans fin");
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    snprintf(root, sizeof(root), "%s/sdscan", argv[1]);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0)
        return 1;
    mkdir(argv[1], 0777);
    mkdir(root, 0777);

    printf("sdscan : un arbre de dossiers\n");
    put("Zelda.nds", 100);
    put("roms/nds/Pong.nds", 2048);
    put("roms/nds/saves/Pong.sav", 10);
    put("roms/nds/notes.txt", 10);
    put("jeux/rpg/Donjon.DSI", 300);          // extension en majuscules
    put("jeux/rpg/profond/a/b/c/Cache.nds", 5);
    put("jeux/arcade/alpha.nds", 7);
    put("_nds/TWiLightMenu/boot.nds", 1);     // dossier système : ignoré
    put("title/00030004/app.nds", 1);
    put("DCIM/photo.nds", 1);
    put(".cache/secret.nds", 1);
    put("$RECYCLE/x.nds", 1);
    put("roms/gba/jeu.gba", 1);               // pas un jeu DS
    put("roms/nds/.hidden.nds", 1);
    put("roms/nds/sans-ext", 1);
    put("a/b/c/d/e/f/g/Limite.nds", 1);       // 7 sous-dossiers : la limite
    put("a/b/c/d/e/f/g/h/Trop-profond.nds", 1); // 8 sous-dossiers : au-delà

    run(1000);
    printf("  %d jeu(x), %d dossier(s)\n", scan.count, scan.dirs);
    CHECK(scan.done && !scan.truncated, "état final");
    CHECK(has("Zelda.nds") && has("roms/nds/Pong.nds") && has("jeux/rpg/Donjon.DSI") &&
          has("jeux/arcade/alpha.nds") && has("jeux/rpg/profond/a/b/c/Cache.nds"), "jeux manquants");
    CHECK(!has("_nds/TWiLightMenu/boot.nds") && !has("title/00030004/app.nds") &&
          !has("DCIM/photo.nds") && !has(".cache/secret.nds") && !has("$RECYCLE/x.nds") &&
          !has("roms/nds/.hidden.nds"), "dossiers système ou cachés non ignorés");
    CHECK(has("a/b/c/d/e/f/g/Limite.nds"), "dossier à la profondeur limite non exploré");
    CHECK(!has("a/b/c/d/e/f/g/h/Trop-profond.nds"), "dossier trop profond exploré");
    CHECK(scan.count == 6, "attendu 6 jeux, trouvé %d", scan.count);

    // Tri par nom sans tenir compte de la casse, tailles justes
    CHECK(strcmp(sdgame_name(&scan.games[0]), "alpha.nds") == 0, "tri : %s", sdgame_name(&scan.games[0]));
    CHECK(strcmp(sdgame_name(&scan.games[2]), "Donjon.DSI") == 0, "tri : %s", sdgame_name(&scan.games[2]));
    CHECK(strcmp(sdgame_name(&scan.games[5]), "Zelda.nds") == 0, "tri : %s", sdgame_name(&scan.games[5]));
    for (int i = 0; i < scan.count; i++)
        if (strcmp(sdgame_name(&scan.games[i]), "Pong.nds") == 0)
            CHECK(scan.games[i].size == 2048, "taille %llu", (unsigned long long)scan.games[i].size);

    char folder[300], want[500];
    for (int i = 0; i < scan.count; i++)
        if (strcmp(sdgame_name(&scan.games[i]), "Pong.nds") == 0)
        {
            sdgame_folder(&scan.games[i], folder, sizeof(folder));
            snprintf(want, sizeof(want), "%s/roms/nds", root);
            CHECK(strcmp(folder, want) == 0, "dossier : %s", folder);
        }

    printf("sdscan : même résultat par petites étapes, et avec une racine terminée par '/'\n");
    sdscan_t first = scan;
    run(1);
    CHECK(scan.count == first.count && scan.dirs == first.dirs, "étapes de 1 entrée");
    for (int i = 0; i < scan.count; i++)
        CHECK(strcmp(scan.games[i].path, first.games[i].path) == 0, "ordre différent");
    char slashed[420];
    snprintf(slashed, sizeof(slashed), "%s/", root);
    sdscan_start(&scan, slashed);
    while (!sdscan_step(&scan, 7)) {}
    CHECK(scan.count == first.count, "racine avec '/' : %d", scan.count);

    printf("sdscan : annulation, dossier absent, limite\n");
    sdscan_start(&scan, root);
    sdscan_step(&scan, 3);
    CHECK(!scan.done, "déjà fini après 3 entrées");
    sdscan_cancel(&scan);
    CHECK(scan.done, "annulation");
    sdscan_start(&scan, "/chemin/qui/n/existe/pas");
    CHECK(scan.done && scan.count == 0, "dossier absent");

    for (int i = 0; i < SDSCAN_MAX + 10; i++)
    {
        char rel[64];
        snprintf(rel, sizeof(rel), "beaucoup/jeu%04d.nds", i);
        put(rel, 1);
    }
    run(1000);
    CHECK(scan.count == SDSCAN_MAX && scan.truncated, "limite : %d jeux, truncated=%d", scan.count, scan.truncated);
    for (int i = 1; i < scan.count; i++)
        CHECK(strcasecmp(sdgame_name(&scan.games[i - 1]), sdgame_name(&scan.games[i])) <= 0, "liste tronquée non triée");

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
