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

    // Wi-Fi -> réseaux enregistrés
    tap(128, 147);
    frames(2);
    tap(128, 47);
    frames(120);
    press(BTN_B);
    frames(2);

    // Remote -> onglet Local : formulaire identifiant / mot de passe
    tap(190, 78);
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
    lremote_net_stop();
        return 0;
}
