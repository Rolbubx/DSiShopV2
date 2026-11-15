// Tests de twlini.c sur PC :  ./test_twlini <dossier>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../source/twlini.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  ÉCHEC : " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static char root[400], ini[600];

static void put(const char *text)
{
    FILE *f = fopen(ini, "wb");
    fputs(text, f);
    fclose(f);
}

static const char *slurp(void)
{
    static char buf[4096];
    FILE *f = fopen(ini, "rb");
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *get(const char *key)
{
    static char val[300];
    return twlini_get(ini, "SRLOADER", key, val, sizeof(val)) ? val : "(absent)";
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    char dir[500];
    snprintf(root, sizeof(root), "%s/twlini", argv[1]);
    mkdir(root, 0777);
    snprintf(dir, sizeof(dir), "%s/_nds", root);
    mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%s/_nds/TWiLightMenu", root);
    mkdir(dir, 0777);
    snprintf(ini, sizeof(ini), "%s/" TWLINI_PATH, root);
    const char *self = "sd:/roms/nds/DSiShop.nds";

    printf("twlini : lancé depuis TWiLight (ROM_PATH déjà sur DSi Shop)\n");
    const char *before =
        "[SRLOADER]\r\nTHEME = 3\r\nROM_PATH = sd:/roms/nds/dsishop.nds\r\n"
        "LAUNCH_TYPE = 1\r\nAUTORUNGAME = 0\r\nHOMEBREW_BOOTSTRAP = 1\r\n"
        "\r\n[GUI]\r\nAUTORUNGAME = 7\r\nLANGUAGE = 2\r\n";
    put(before);
    CHECK(!twlini_autoboot_get(root, self), "déjà actif ?");
    CHECK(twlini_autoboot_set(root, self, true), "activation");
    CHECK(twlini_autoboot_get(root, self), "pas actif après activation");
    CHECK(strcmp(get("LAUNCH_TYPE"), "1") == 0, "LAUNCH_TYPE modifié : %s", get("LAUNCH_TYPE"));
    CHECK(strcmp(get("THEME"), "3") == 0 && strcmp(get("HOMEBREW_BOOTSTRAP"), "1") == 0,
          "autres réglages modifiés");
    CHECK(strstr(slurp(), "[GUI]\r\nAUTORUNGAME = 7\r\nLANGUAGE = 2\r\n"), "section [GUI] abîmée");
    CHECK(twlini_autoboot_set(root, self, false), "désactivation");
    CHECK(strcmp(slurp(), before) == 0, "fichier différent après aller-retour :\n%s", slurp());

    printf("twlini : une autre ROM était la dernière lancée\n");
    put("[SRLOADER]\nROM_PATH=sd:/roms/nds/Autre.nds\nLAUNCH_TYPE=1\nPREVIOUS_USED_DEVICE=1\n"
        "SLOT1_LAUNCHED=1\n[AUTRE]\nX=1\n");
    CHECK(twlini_autoboot_set(root, self, true), "activation");
    CHECK(twlini_autoboot_get(root, self), "pas actif");
    CHECK(strcmp(get("ROM_PATH"), self) == 0 && strcmp(get("LAUNCH_TYPE"), "2") == 0 &&
          strcmp(get("PREVIOUS_USED_DEVICE"), "0") == 0 && strcmp(get("SLOT1_LAUNCHED"), "0") == 0,
          "réglages de lancement : %s", slurp());
    CHECK(strstr(slurp(), "[AUTRE]\r\nX=1\r\n"), "section suivante abîmée");
    CHECK(strstr(slurp(), "AUTORUNGAME = 1\r\n[AUTRE]"), "clé ajoutée hors de la section");
    // Un autre jeu lancé ensuite : ce n'est plus DSi Shop qui démarre
    const char *const k[] = { "ROM_PATH" }, *const v[] = { "sd:/roms/nds/Autre.nds" };
    CHECK(twlini_set(ini, "SRLOADER", k, v, 1) && !twlini_autoboot_get(root, self),
          "encore actif alors qu'une autre ROM est la dernière");

    printf("twlini : fichier sans section, puis absent\n");
    put("[GUI]\r\nLANGUAGE = 2\r\n");
    CHECK(twlini_autoboot_set(root, self, true) && twlini_autoboot_get(root, self), "sans section");
    CHECK(strstr(slurp(), "[GUI]\r\nLANGUAGE = 2\r\n[SRLOADER]\r\n"), "section mal ajoutée");
    remove(ini);
    CHECK(!twlini_autoboot_get(root, self), "actif sans fichier");
    CHECK(twlini_autoboot_set(root, self, true) && twlini_autoboot_get(root, self), "fichier absent");
    CHECK(!twlini_autoboot_set(root, "", true), "chemin vide accepté");

    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.dsitmp", ini);
    struct stat st;
    CHECK(stat(tmp, &st) != 0, "fichier temporaire restant");

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
