// Simulateur PC : exécute app.c avec des entrées scriptées et enregistre des
// captures des deux écrans (format PPM, empilés comme sur la console).
//
//   make -C tools/sim && ./tools/sim/dsishop_sim out_dir

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../../source/app.h"
#include "../../source/games.h"
#include "../../source/lremote.h"
#include "../../source/lremote_glue.h"
#include "../../source/gfx.h"
#include "../../source/input.h"
#include "../../source/net.h"

static uint16_t screens[2][SCREEN_W * SCREEN_H];

void gfx_hw_init(void) {}

void gfx_hw_present(screen_id_t screen, const uint16_t *pixels)
{
    memcpy(screens[screen], pixels, sizeof(screens[screen]));
}

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    const int gap = 8;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H * 2 + gap);
    for (int s = 0; s < 2; s++)
    {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        {
            uint16_t c = screens[s][i];
            unsigned char rgb[3] = {
                (unsigned char)((c & 31) * 255 / 31),
                (unsigned char)(((c >> 5) & 31) * 255 / 31),
                (unsigned char)(((c >> 10) & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
        if (s == 0)
            for (int i = 0; i < SCREEN_W * gap; i++)
                fwrite("\x10\x10\x10", 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

static input_t in;

static void frames(int n)
{
    for (int i = 0; i < n; i++)
    {
        gfx_present();
        net_update();
        app_frame(&in);
        uint32_t held = in.held;
        memset(&in, 0, sizeof(in));
        in.held = held & ~BTN_TOUCH;
    }
    gfx_present();
}

static void tap(int x, int y)
{
    in.down = BTN_TOUCH; in.held = BTN_TOUCH; in.touching = true; in.tx = x; in.ty = y;
    frames(1);
    in.up = BTN_TOUCH; in.tx = x; in.ty = y;
    frames(1);
}

static void press(uint32_t key)
{
    in.down = key; in.repeat = key; in.held = key;
    frames(1);
    in.up = key;
    frames(1);
}

extern char sim_games_dir[512];
extern char sim_root[512];
void remote_sim_pair(const char *name, const char *mail);

static void fake_rom(const char *name, size_t size)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", sim_games_dir, name);
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; f && i < size; i++)
        fputc(0, f);
    if (f)
        fclose(f);
}

// Faux jeu n'importe où sur la "carte" (dossiers créés au besoin)
static void fake_game_at(const char *rel, size_t size)
{
    char path[700], dir[700];
    snprintf(path, sizeof(path), "%s/%s", sim_root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + strlen(sim_root) + 1; *p; p++)
        if (*p == '/')
        {
            *p = '\0';
            mkdir(dir, 0777);
            *p = '/';
        }
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; f && i < size; i += 16)
        fwrite("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 1, size - i < 16 ? size - i : 16, f);
    if (f)
        fclose(f);
}

static void record(const char *file, const char *source, const char *version,
                   const char *url)
{
    game_t g = { 0 };
    snprintf(g.file, sizeof(g.file), "%s", file);
    snprintf(g.source, sizeof(g.source), "%s", source);
    snprintf(g.version, sizeof(g.version), "%s", version);
    snprintf(g.url, sizeof(g.url), "%s", url);
    games_record(sim_games_dir, &g);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    // Dossier de jeux factice (vrais fichiers : la liste, les mises à jour et
    // les sauvegardes passent par games.c, comme sur la console)
    snprintf(sim_root, sizeof(sim_root), "%s", dir);
    snprintf(sim_games_dir, sizeof(sim_games_dir), "%s/roms", dir);
    mkdir(sim_games_dir, 0777);
    char saves[600];
    snprintf(saves, sizeof(saves), "%s/saves", sim_games_dir);
    mkdir(saves, 0777);
    fake_rom("Pong.nds", 131072);
    fake_rom("Pong.sav", 512);
    fake_rom("saves/Pong.sav", 512);
    fake_rom("Tetris DS Remix.dsi", 4718592);
    fake_rom("Zelda Homebrew.nds", 9437184);
    // Jeux rangés ailleurs sur la carte (dossiers libres), et un dossier système à ignorer
    fake_game_at("homebrew/Doom DS.nds", 3145728);
    fake_game_at("jeux/Plateforme/Mario Clone.nds", 6291456);
    fake_game_at("jeux/Puzzle/Tetris Plus.dsi", 1048576);
    fake_game_at("_nds/TWiLightMenu/menu.nds", 4096);
    record("Pong.nds", "github.com/auteur/pong", "v1.0.3",
           "https://github.com/auteur/pong/releases/download/v1.0.3/pong.nds");
    record("Zelda Homebrew.nds", "https://exemple.fr/zelda.zip", "",
           "https://exemple.fr/zelda.zip");

    gfx_init();
    app_init(net_init());
    // Remote local : vraie configuration (fichier dans le dossier de sortie)
    lremote_glue_init();
    frames(2);
    save(dir, "01_menu");

    // Gérer les jeux : tous les jeux de la carte SD, puis la fiche d'un jeu rangé ailleurs
    tap(190, 70);
    frames(40);
    save(dir, "01b_gerer_les_jeux");
    tap(128, 68);
    frames(3);
    save(dir, "01c_fiche_jeu_sd");
    tap(71, 56);               // Lancer
    frames(8);
    save(dir, "01d_lancement");
    press(BTN_B);              // B pendant le court délai : annule
    frames(3);
    press(BTN_B);              // Fiche -> liste
    frames(3);
    press(BTN_B);              // Liste -> accueil
    frames(3);

    // Wi-Fi -> réseaux enregistrés
    tap(60, 126);              // Wi-Fi
    frames(2);
    tap(128, 47);
    frames(120);
    press(BTN_B);
    frames(2);

    // Remote -> onglet Local : formulaire identifiant / mot de passe
    tap(128, 172);             // Case Remote
    frames(2);
    save(dir, "02_remote_local_config");

    tap(128, 74);              // Champ identifiant -> clavier
    frames(2);
    tap(4 + 12, 88 + 12);      // a
    tap(4 + 25 + 12, 88 + 12); // z
    tap(4 + 50 + 12, 88 + 12); // e
    press(BTN_START);          // Valider
    frames(2);
    tap(128, 114);             // Champ mot de passe -> clavier
    frames(2);
    for (int k = 0; k < 6; k++)
        tap(4 + k * 25 + 12, 88 + 12);   // azerty
    save(dir, "03_saisie_mot_de_passe");
    press(BTN_START);
    frames(2);
    save(dir, "04_formulaire_rempli");
    tap(128, 146);             // Enregistrer et activer
    frames(3);
    // Démarre le vrai serveur sur un port de test (le simulateur n'appelle
    // pas lremote_glue_update, qui écouterait sur le port 80)
    lremote_net_start(18080);
    frames(3);
    save(dir, "05_remote_local_qr");

    // Onglet En ligne
    tap(188, 36);
    frames(25);
    save(dir, "06_remote_en_ligne");
    press(BTN_B);
    frames(2);
    save(dir, "07_menu_remote_actif");

    // Plus -> Réglages (thème, Verbose, démarrage)
    press(BTN_B);
    frames(2);
    tap(178, 126);             // Raccourci "Plus"
    frames(2);
    save(dir, "08_plus");
    tap(128, 67);              // Réglages
    frames(2);
    save(dir, "08b_reglages");
    tap(128, 107);             // Au démarrage : bascule sur DSi Shop
    frames(2);
    save(dir, "08c_reglages_demarrage");
    tap(128, 139);             // Retour
    frames(200);               // Laisse le message disparaître

    // Plus -> Mettre à jour TWiLight Menu++
    tap(128, 40);
    frames(30);
    save(dir, "09_twilight_version");
    tap(128, 119);             // Mettre à jour
    frames(80);
    save(dir, "10_twilight_paquets");
    frames(120);
    save(dir, "11_twilight_termine");

    // Plus -> À propos -> mise à jour de DSi Shop
    press(BTN_B);
    frames(2);
    tap(128, 121);             // À propos
    frames(2);
    tap(128, 117);             // Rechercher une mise à jour
    frames(30);
    save(dir, "12_a_propos_nouvelle_version");
    tap(128, 117);             // Mettre à jour DSi Shop
    frames(30);
    save(dir, "13_a_propos_telechargement");
    frames(60);
    save(dir, "14_a_propos_termine");

    // Installation SANS Verbose : pré-description simple, puis annulation
    press(BTN_B);              // À propos -> Plus
    frames(2);
    tap(128, 67);              // Réglages
    frames(2);
    tap(128, 75);              // Verbose : NON
    frames(2);
    save(dir, "15_verbose_non");
    press(BTN_B);              // -> Plus
    frames(2);
    press(BTN_B);              // -> accueil
    frames(2);
    tap(66, 78);               // Installer un jeu
    frames(2);
    tap(70, 113);              // Installer
    frames(10);
    save(dir, "16_install_resolution");
    frames(30);
    save(dir, "17_preview_simple");
    tap(128, 149);             // Annuler
    frames(5);

    // Installation AVEC Verbose (réglage par défaut) : confirmation puis journal
    press(BTN_B);              // -> accueil
    frames(2);
    tap(178, 126);             // Plus
    frames(2);
    tap(128, 67);              // Réglages
    frames(2);
    tap(128, 75);              // Verbose : OUI
    frames(2);
    save(dir, "18_verbose_oui");
    press(BTN_B);
    frames(2);
    press(BTN_B);
    frames(2);
    tap(66, 78);               // Installer un jeu
    frames(2);
    tap(70, 113);              // Installer
    frames(40);
    save(dir, "19_preview_detaillee");
    tap(128, 113);             // Confirmer
    frames(90);
    save(dir, "20_install_detaillee");
    frames(200);
    save(dir, "21_install_terminee");
    lremote_net_stop();
        return 0;
}
