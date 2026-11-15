// Tests de twlini.c sur PC :  ./test_twlini <dossier>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

// Contenu d'un fichier (tampon statique : valable jusqu'au prochain appel)
static const char *read_file(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
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


// ---------------------------------------------------------------------------
// Fausses ROM : en-tête de 0x200 octets + code ARM9 + code ARM7
// ---------------------------------------------------------------------------

typedef struct {
    const char *code;       // Code du jeu (4 caractères)
    uint8_t unit;
    uint32_t access;
    uint32_t sig[4];        // Début du code ARM9
    uint32_t a9dst;
    uint32_t a7exec, a7dst;
    uint32_t a7word_c8;     // Mot lu à l'offset 0xC8 du code ARM7
    int sdreg;              // Le code ARM7 contient le registre de la SD de la DSi
    bool bad_crc;
} fake_rom_t;

static void w32(uint8_t *b, size_t off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

static uint16_t crc16_hdr(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

static void write_rom(const char *path, const fake_rom_t *r)
{
    static uint8_t buf[0x9000];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "HOMEBREW", 8);
    memcpy(buf + 0x0C, r->code, 4);
    buf[0x12] = r->unit;
    const uint32_t a9off = 0x4000, a9exec = 0x02000800, a7off = 0x8000;
    w32(buf, 0x20, a9off);
    w32(buf, 0x24, a9exec);
    w32(buf, 0x28, r->a9dst);
    w32(buf, 0x2C, 0x1000);
    w32(buf, 0x30, a7off);
    w32(buf, 0x34, r->a7exec);
    w32(buf, 0x38, r->a7dst);
    w32(buf, 0x3C, 0x800);
    w32(buf, 0x1B4, r->access);
    for (int i = 0; i < 4; i++)
        w32(buf, a9off + (a9exec - 0x02000000) + (size_t)i * 4, r->sig[i]);
    w32(buf, a7off + 0xC8, r->a7word_c8);
    if (r->sdreg)
        w32(buf, a7off + 0x340, 0x04004800);
    uint16_t crc = crc16_hdr(buf, 0x15E);
    if (r->bad_crc)
        crc ^= 0x1234;
    buf[0x15E] = (uint8_t)crc;
    buf[0x15F] = (uint8_t)(crc >> 8);
    FILE *f = fopen(path, "wb");
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
}

static const fake_rom_t SDK2 = { "ABCD", 0, 0, { 0xE3A0C301, 0xE58CC208, 0xEB000010, 0xE3A00000 },
                                 0x02000000, 0x02380000, 0x02380000, 0, 0, false };
static const fake_rom_t SDK3 = { "ABCD", 0, 0, { 0xE3A0C301, 0xE58CC208, 0xE1DC00B6, 0xE3500000 },
                                 0x02000000, 0x02380000, 0x02380000, 0, 0, false };
static const fake_rom_t LIBNDS_MODERN = { "####", 0, 0, { 0xE3A00301, 0xE5800208, 0xE3A00013, 0xE129F000 },
                                          0x02000000, 0x02380000, 0x02380000, 0, 0, false };
static const fake_rom_t LIBNDS_OLD = { "####", 0, 0, { 0xE3A00301, 0xE5800208, 0xE3A00013, 0xE129F000 },
                                       0x02000000, 0x037F8000, 0x037F8000, 0x037F8100, 0, false };

static const char *kind_name(rom_kind_t k)
{
    static const char *const n[] = { "INVALID", "COMMERCIAL", "HOMEBREW_MODERN", "HOMEBREW_OLD", "DSIWARE" };
    return n[k];
}

static void expect_kind(const char *dir, const char *what, const fake_rom_t *r, rom_kind_t want)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.nds", dir, what);
    write_rom(path, r);
    rom_kind_t got = twlini_classify_rom(path);
    CHECK(got == want, "%s : attendu %s, obtenu %s", what, kind_name(want), kind_name(got));
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    char dir[500], cmd[600];
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


    // -----------------------------------------------------------------------
    printf("twlini : reconnaissance du type de ROM (règles de TWiLight Menu++)\n");
    char kdir[500];
    snprintf(kdir, sizeof(kdir), "%s/twlrom", argv[1]);
    mkdir(kdir, 0777);
    expect_kind(kdir, "sdk2", &SDK2, ROM_COMMERCIAL);
    expect_kind(kdir, "sdk3", &SDK3, ROM_COMMERCIAL);
    fake_rom_t r = SDK2;
    r.unit = 2;                                   // DSi amélioré, pas DSiWare
    expect_kind(kdir, "dsi-enhanced", &r, ROM_COMMERCIAL);
    r.unit = 3;
    r.access = 0x10;                              // Bit 4 : DSiWare
    expect_kind(kdir, "dsiware", &r, ROM_DSIWARE);
    r = SDK2;
    r.sig[2] = 0xEAFFFFFF;
    r.sig[3] = 0xE1DC00B6;                        // SDK 4
    expect_kind(kdir, "sdk4", &r, ROM_COMMERCIAL);
    r = LIBNDS_MODERN;
    expect_kind(kdir, "libnds-moderne", &r, ROM_HOMEBREW_MODERN);
    expect_kind(kdir, "libnds-ancien", &LIBNDS_OLD, ROM_HOMEBREW_OLD);
    r = LIBNDS_OLD;
    r.sdreg = 1;                                  // Lit le registre de la SD de la DSi
    expect_kind(kdir, "libnds-ancien-sd", &r, ROM_HOMEBREW_MODERN);
    r = LIBNDS_OLD;
    r.a7word_c8 = 0;                              // Adresse hors de l'IWRAM : moderne
    expect_kind(kdir, "libnds-adresse", &r, ROM_HOMEBREW_MODERN);
    r = LIBNDS_MODERN;
    r.sig[0] = 0x11111111;                        // Début de code inconnu : homebrew
    expect_kind(kdir, "inconnu", &r, ROM_HOMEBREW_MODERN);
    r.a7exec = r.a7dst = 0x037F8000;
    expect_kind(kdir, "inconnu-ancien", &r, ROM_HOMEBREW_OLD);
    r = LIBNDS_MODERN;
    r.sig[0] = 0x11111111;
    r.code = "HNAA";                              // Modcrypt : pas un homebrew
    expect_kind(kdir, "hna", &r, ROM_COMMERCIAL);
    r = SDK2;
    r.bad_crc = true;
    expect_kind(kdir, "mauvais-crc", &r, ROM_INVALID);
    r = SDK2;
    r.a9dst = 0x08000000;                         // Adresse ARM9 absurde
    expect_kind(kdir, "mauvaise-adresse", &r, ROM_INVALID);
    {
        char path[600];
        snprintf(path, sizeof(path), "%s/absent.nds", kdir);
        CHECK(twlini_classify_rom(path) == ROM_INVALID, "fichier absent");
        snprintf(path, sizeof(path), "%s/court.nds", kdir);
        FILE *f = fopen(path, "wb");
        fputs("trop court", f);
        fclose(f);
        CHECK(twlini_classify_rom(path) == ROM_INVALID, "fichier trop court");
    }

    // -----------------------------------------------------------------------
    printf("twlini : régler TWiLight Menu++ pour lancer un jeu, puis tout remettre\n");
    char lroot[500], lini[600], rom[600], prev[600], lv[300];
    snprintf(lroot, sizeof(lroot), "%s/twlaunch", argv[1]);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", lroot);
    if (system(cmd) != 0)
        return 1;
    mkdir(lroot, 0777);
    snprintf(lini, sizeof(lini), "%s/_nds", lroot);
    mkdir(lini, 0777);
    snprintf(lini, sizeof(lini), "%s/_nds/TWiLightMenu", lroot);
    mkdir(lini, 0777);
    snprintf(lini, sizeof(lini), "%s/" TWLINI_PATH, lroot);
    snprintf(rom, sizeof(rom), "%s/roms", lroot);
    mkdir(rom, 0777);
    snprintf(rom, sizeof(rom), "%s/roms/Jeu.nds", lroot);
    snprintf(prev, sizeof(prev), "%s/_dsishop/prev_autorun", lroot);

    const char *original = "[SRLOADER]\r\nTHEME = 3\r\nAUTORUNGAME = 0\r\nROM_PATH = sd:/roms/Autre.nds\r\n"
                           "LAUNCH_TYPE = 1\r\n\r\n[GUI]\r\nLANGUAGE = 2\r\n";
    FILE *lf = fopen(lini, "wb");
    fputs(original, lf);
    fclose(lf);

    #define LGET(key) (twlini_get(lini, "SRLOADER", key, lv, sizeof(lv)) ? lv : "(absent)")
    char keep[4096];

    // Jeu du commerce : nds-bootstrap
    write_rom(rom, &SDK2);
    rom_kind_t kind = ROM_INVALID;
    CHECK(twlini_launch_set(lroot, rom, &kind) == LAUNCH_OK && kind == ROM_COMMERCIAL, "jeu du commerce");
    CHECK(!strcmp(LGET("AUTORUNGAME"), "1") && !strcmp(LGET("ROM_PATH"), rom) &&
          !strcmp(LGET("LAUNCH_TYPE"), "1") && !strcmp(LGET("HOMEBREW_BOOTSTRAP"), "0") &&
          !strcmp(LGET("PREVIOUS_USED_DEVICE"), "0") && !strcmp(LGET("SLOT1_LAUNCHED"), "0"),
          "réglages d'un jeu du commerce :\n%s", read_file(lini));
    CHECK(!strcmp(LGET("THEME"), "3"), "autres réglages modifiés");
    CHECK(!strcmp(read_file(prev), "0"), "ancien réglage noté : '%s'", read_file(prev));

    // Homebrew moderne : lancement direct ; ancien : nds-bootstrap (hb)
    write_rom(rom, &LIBNDS_MODERN);
    CHECK(twlini_launch_set(lroot, rom, &kind) == LAUNCH_OK && kind == ROM_HOMEBREW_MODERN, "homebrew moderne");
    CHECK(!strcmp(LGET("LAUNCH_TYPE"), "2") && !strcmp(LGET("HOMEBREW_BOOTSTRAP"), "1"), "réglages homebrew moderne");
    write_rom(rom, &LIBNDS_OLD);
    CHECK(twlini_launch_set(lroot, rom, &kind) == LAUNCH_OK && kind == ROM_HOMEBREW_OLD, "homebrew ancien");
    CHECK(!strcmp(LGET("LAUNCH_TYPE"), "1") && !strcmp(LGET("HOMEBREW_BOOTSTRAP"), "1"), "réglages homebrew ancien");
    CHECK(!strcmp(read_file(prev), "0"), "lancements enchaînés : la note d'origine est écrasée : '%s'", read_file(prev));

    // Tout remettre : même fichier qu'au départ, sauf la dernière ROM lancée
    CHECK(twlini_launch_restore(lroot), "restauration");
    CHECK(!strcmp(LGET("AUTORUNGAME"), "0"), "AUTORUNGAME remis : %s", LGET("AUTORUNGAME"));
    CHECK(access(prev, F_OK) != 0, "note non supprimée");
    CHECK(!twlini_launch_restore(lroot), "rien à remettre une 2e fois");
    CHECK(!strcmp(LGET("THEME"), "3"), "THEME");

    // Si l'autorun était déjà actif, on le remet actif
    snprintf(keep, sizeof(keep), "%s", read_file(lini));
    char *p = strstr(keep, "AUTORUNGAME = 0");
    CHECK(p != NULL, "ligne AUTORUNGAME");
    if (p)
        p[14] = '1';
    lf = fopen(lini, "wb");
    fputs(keep, lf);
    fclose(lf);
    write_rom(rom, &SDK2);
    CHECK(twlini_launch_set(lroot, rom, NULL) == LAUNCH_OK, "lancement avec autorun déjà actif");
    CHECK(!strcmp(read_file(prev), "1"), "note : '%s'", read_file(prev));
    CHECK(twlini_launch_restore(lroot) && !strcmp(LGET("AUTORUNGAME"), "1"), "autorun remis à 1");

    // Refus : rien n'est modifié, aucune note laissée
    char snap[4096];
    snprintf(snap, sizeof(snap), "%s", read_file(lini));
    char other[600];
    snprintf(other, sizeof(other), "%s/roms/Ware.nds", lroot);
    fake_rom_t ware = SDK2;
    ware.unit = 3;
    ware.access = 0x10;
    write_rom(other, &ware);
    CHECK(twlini_launch_set(lroot, other, NULL) == LAUNCH_DSIWARE, "DSiWare refusé");
    snprintf(other, sizeof(other), "%s/roms/Casse.nds", lroot);
    fake_rom_t bad = SDK2;
    bad.bad_crc = true;
    write_rom(other, &bad);
    CHECK(twlini_launch_set(lroot, other, NULL) == LAUNCH_BAD_ROM, "ROM invalide refusée");
    CHECK(twlini_launch_set(lroot, "/ailleurs/Jeu.nds", NULL) == LAUNCH_NOT_SD, "chemin hors de la carte");
    snprintf(other, sizeof(other), "%s/roms/Absent.nds", lroot);
    CHECK(twlini_launch_set(lroot, other, NULL) == LAUNCH_BAD_ROM, "ROM absente");
    CHECK(strcmp(read_file(lini), snap) == 0, "réglages modifiés par un lancement refusé");
    CHECK(access(prev, F_OK) != 0, "note laissée par un lancement refusé");

    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.dsitmp", ini);
    struct stat st;
    CHECK(stat(tmp, &st) != 0, "fichier temporaire restant");

    printf(failures ? "\n%d ÉCHEC(S)\n" : "\nTout est OK\n", failures);
    return failures ? 1 : 0;
}
