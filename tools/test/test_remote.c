// Tests de games.c et remote_proto.c sur PC.
//   make -C tools/test check

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../source/games.h"
#include "../../source/remote_proto.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC (ligne %d) : ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static void touch(const char *dir, const char *name, size_t size)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; i < size; i++)
        fputc((int)i, f);
    fclose(f);
}

static bool rec(const char *dir, const char *file, const char *source,
                const char *version, const char *url)
{
    game_t g = { 0 };
    snprintf(g.file, sizeof(g.file), "%s", file);
    snprintf(g.source, sizeof(g.source), "%s", source);
    snprintf(g.version, sizeof(g.version), "%s", version);
    snprintf(g.url, sizeof(g.url), "%s", url ? url : "");
    snprintf(g.etag, sizeof(g.etag), "\"abc\"");
    snprintf(g.modified, sizeof(g.modified), "Wed, 11 Nov 2026 10:00:00 GMT");
    return games_record(dir, &g);
}

static bool exists(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

static long file_size(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_games(const char *base)
{
    printf("games : liste, registre, désinstallation\n");

    char dir[600];
    snprintf(dir, sizeof(dir), "%s/roms", base);
    mkdir(base, 0777);
    mkdir(dir, 0777);

    touch(dir, "zelda.nds", 100);
    touch(dir, "Pong.NDS", 200);
    touch(dir, "Menu.dsi", 300);
    touch(dir, "notes.txt", 10);
    touch(dir, ".cache.nds", 10);

    CHECK(rec(dir, "Pong.NDS", "github.com/a/pong", "v1.0",
              "https://github.com/a/pong/releases/download/v1.0/pong.nds"), "record");
    CHECK(rec(dir, "zelda.nds", "https://x/zelda.zip", "v2", "https://x/zelda.zip"), "record");
    CHECK(rec(dir, "Pong.NDS", "github.com/a/pong", "v1.1",
              "https://github.com/a/pong/releases/download/v1.1/pong.nds"), "record (mise à jour)");
    CHECK(!rec(dir, "../evil.nds", "x", "y", NULL), "nom dangereux accepté");

    // Ancien format du registre (3 colonnes) toujours lisible
    {
        char path[700];
        snprintf(path, sizeof(path), "%s/.dsishop.tsv", dir);
        FILE *f = fopen(path, "a");
        fprintf(f, "Menu.dsi\tgithub.com/old/menu\tv0.1\n");
        fclose(f);
    }

    game_t g[GAMES_MAX];
    int n = games_scan(dir, g, GAMES_MAX);
    CHECK(n == 3, "3 jeux attendus, %d trouvés", n);
    if (n == 3)
    {
        CHECK(strcmp(g[0].file, "Menu.dsi") == 0 && strcmp(g[1].file, "Pong.NDS") == 0 &&
              strcmp(g[2].file, "zelda.nds") == 0, "ordre : %s %s %s", g[0].file, g[1].file, g[2].file);
        CHECK(g[1].size == 200, "taille");
        CHECK(strcmp(g[1].version, "v1.1") == 0, "version mise à jour : %s", g[1].version);
        CHECK(strcmp(g[2].source, "https://x/zelda.zip") == 0, "source");
        CHECK(strcmp(g[1].url, "https://github.com/a/pong/releases/download/v1.1/pong.nds") == 0,
              "url : %s", g[1].url);
        CHECK(strcmp(g[1].etag, "\"abc\"") == 0, "etag");
        CHECK(strcmp(g[0].source, "github.com/old/menu") == 0 && g[0].url[0] == '\0',
              "ancien format : %s", g[0].source);
        CHECK(games_is_tracked(&g[1]) && games_is_tracked(&g[0]), "suivi");
    }

    // --- Version précédente (mise à jour puis retour en arrière) ---
    touch(dir, "zelda.nds", 111);                     // "v2"
    CHECK(!games_has_backup(dir, "zelda.nds"), "pas encore de copie");
    CHECK(games_backup(dir, "zelda.nds"), "copie de l'ancienne version");
    CHECK(!exists(dir, "zelda.nds"), "la ROM a été déplacée");
    CHECK(games_has_backup(dir, "zelda.nds"), "copie présente");
    touch(dir, "zelda.nds", 222);                     // "v3" installée
    CHECK(games_restore_backup(dir, "zelda.nds"), "restauration");
    CHECK(file_size(dir, "zelda.nds") == 111, "ancienne version remise (%ld)", file_size(dir, "zelda.nds"));
    CHECK(file_size(dir, ".dsishop_old/zelda.nds") == 222, "la nouvelle est gardée de côté");
    game_t r;
    CHECK(games_get_record(dir, "zelda.nds", &r) && r.version[0] == '\0' &&
          strcmp(r.url, "https://x/zelda.zip") == 0, "version effacée, adresse gardée");
    CHECK(!games_backup(dir, "absent.nds"), "copie d'un fichier absent");

    // --- Sauvegardes ---
    char saves_dir[700];
    snprintf(saves_dir, sizeof(saves_dir), "%s/saves", dir);
    mkdir(saves_dir, 0777);
    touch(dir, "zelda.sav", 512);
    touch(saves_dir, "zelda.sav", 512);
    touch(saves_dir, "zelda.sav2", 512);
    touch(saves_dir, "zelda2.sav", 512);              // Autre jeu : à garder
    touch(dir, "Menu.pub", 16);
    char paths[GAMES_MAX_SAVES][320];
    CHECK(games_find_saves(dir, "zelda.nds", paths, GAMES_MAX_SAVES) == 3, "3 sauvegardes");
    CHECK(games_find_saves(dir, "Menu.dsi", NULL, 0) == 1, "sauvegarde .pub");
    CHECK(games_find_saves(dir, "../x.nds", NULL, 0) == 0, "nom dangereux");
    CHECK(games_delete_saves(dir, "zelda.nds") == 3, "suppression");
    CHECK(games_find_saves(dir, "zelda.nds", NULL, 0) == 0, "plus de sauvegardes");
    CHECK(exists(saves_dir, "zelda2.sav"), "la sauvegarde d'un autre jeu est intacte");

    // La désinstallation garde les sauvegardes mais supprime la copie
    touch(dir, "zelda.sav", 512);
    CHECK(games_uninstall(dir, "zelda.nds"), "désinstallation zelda");
    CHECK(!games_has_backup(dir, "zelda.nds"), "copie supprimée");
    CHECK(exists(dir, "zelda.sav"), "sauvegarde gardée");
    touch(dir, "zelda.nds", 100);
    CHECK(games_scan(dir, g, GAMES_MAX) == 3, "rescan");

    CHECK(!games_uninstall(dir, "notes.txt"), "un .txt ne doit pas être supprimable");
    CHECK(!games_uninstall(dir, "../roms/Pong.NDS"), "chemin avec dossier refusé");
    CHECK(!games_uninstall(dir, "absent.nds"), "fichier absent");
    CHECK(games_uninstall(dir, "Pong.NDS"), "désinstallation");

    n = games_scan(dir, g, GAMES_MAX);
    CHECK(n == 2, "2 jeux restants, %d", n);
    CHECK(!exists(dir, "Pong.NDS"), "Pong supprimé");

    // L'entrée du registre a disparu : on réinstalle sans source
    touch(dir, "Pong.NDS", 50);
    n = games_scan(dir, g, GAMES_MAX);
    CHECK(n == 3 && g[1].source[0] == '\0', "registre nettoyé");

    char path[700];
    snprintf(path, sizeof(path), "%s/notes.txt", dir);
    struct stat st;
    CHECK(stat(path, &st) == 0, "notes.txt doit être intact");
}

static void test_parse(void)
{
    printf("remote_proto : lecture des réponses du serveur\n");

    rreply_t r;
    remote_parse_reply(
        "device\t0123456789abcdef\t" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "pair\tABCD2345\thttps://exemple.fr/p/ABCD2345\t600\n", &r);
    CHECK(r.has_device && strcmp(r.device_id, "0123456789abcdef") == 0, "device");
    CHECK(strlen(r.device_token) == 64, "jeton");
    CHECK(r.has_pair && strcmp(r.pair_code, "ABCD2345") == 0 && r.pair_ttl == 600, "pair");
    CHECK(strcmp(r.pair_url, "https://exemple.fr/p/ABCD2345") == 0, "url");
    CHECK(!r.owner_known, "pas de owner");

    remote_parse_reply("owner\t-\n", &r);
    CHECK(r.owner_known && !r.has_owner, "owner absent");

    remote_parse_reply(
        "owner\tAlice Martin\talice@example.com\r\n"
        "cmd\tc1\tinstall\tgithub.com/a/b\n"
        "cmd\tc2\tuninstall\tMon Jeu.nds\n"
        "cmd\tc3\treboot\t\n"
        "cmd\tc4\tformat\tsd:\n"
        "cmd\tc5\tupdate\tPong.nds\n"
        "ligne bizarre\n", &r);
    CHECK(r.has_owner && strcmp(r.owner_name, "Alice Martin") == 0, "owner");
    CHECK(strcmp(r.owner_email, "alice@example.com") == 0, "email (\\r retiré)");
    CHECK(r.ncmds == 5, "5 commandes, %d", r.ncmds);
    CHECK(r.cmds[4].type == RCMD_UPDATE && strcmp(r.cmds[4].arg, "Pong.nds") == 0, "update");
    CHECK(r.cmds[0].type == RCMD_INSTALL && strcmp(r.cmds[0].arg, "github.com/a/b") == 0, "install");
    CHECK(r.cmds[1].type == RCMD_UNINSTALL && strcmp(r.cmds[1].arg, "Mon Jeu.nds") == 0, "uninstall");
    CHECK(r.cmds[2].type == RCMD_REBOOT, "reboot");
    CHECK(r.cmds[3].type == RCMD_UNKNOWN, "commande inconnue");

    // Trop de commandes : on garde les premières
    char big[4096] = "";
    for (int i = 0; i < 20; i++)
    {
        char l[64];
        snprintf(l, sizeof(l), "cmd\tid%d\tcancel\n", i);
        strcat(big, l);
    }
    remote_parse_reply(big, &r);
    CHECK(r.ncmds == RREPLY_MAX_CMDS, "limite de commandes");

    remote_parse_reply("", &r);
    CHECK(!r.owner_known && r.ncmds == 0, "réponse vide");
}

static void test_build(void)
{
    printf("remote_proto : construction du statut\n");

    game_t games[2] = { 0 };
    strcpy(games[0].file, "Pong.nds");
    games[0].size = 131072;
    strcpy(games[0].source, "github.com/a/pong");
    strcpy(games[0].version, "v1.0");
    strcpy(games[0].url, "https://github.com/a/pong/releases/download/v1.0/pong.nds");
    strcpy(games[1].file, "Bad\tName.nds");
    games[1].size = 5000000000ull;

    rack_t acks[1] = { { "c1", true, "Installation\nlancée" } };
    rstatus_t st = {
        .state = "downloading", .message = "Téléchargement…", .file = "jeu.zip",
        .release = "v2", .version = "0.3.0", .now = 1024, .total = 6000000000ull,
        .free_bytes = 123,
    };

    char buf[2048];
    size_t len = remote_build_status(buf, sizeof(buf), &st, acks, 1, games, 2);
    const char *expected =
        "state\tdownloading\n"
        "message\tTéléchargement…\n"
        "progress\t1024\t6000000000\n"
        "file\tjeu.zip\n"
        "release\tv2\n"
        "version\t0.3.0\n"
        "free\t123\n"
        "ack\tc1\tok\tInstallation lancée\n"
        "game\tPong.nds\t131072\tgithub.com/a/pong\tv1.0\t"
        "https://github.com/a/pong/releases/download/v1.0/pong.nds\n"
        "game\tBad Name.nds\t5000000000\t\t\t\n";
    CHECK(strcmp(buf, expected) == 0, "statut inattendu :\n%s", buf);
    CHECK(len == strlen(expected), "longueur");

    // Buffer trop petit : pas de débordement, chaîne terminée
    char small[40];
    memset(small, 'X', sizeof(small));
    len = remote_build_status(small, sizeof(small), &st, acks, 1, games, 2);
    CHECK(len < sizeof(small) && small[len] == '\0', "troncature propre (len %zu)", len);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/dsishop_tests";
    char base[512];
    snprintf(base, sizeof(base), "%s/games_test", dir);

    test_games(base);
    test_parse();
    test_build();

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
