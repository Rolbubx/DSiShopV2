// Simulateur PC : exécute app.c avec des entrées scriptées et enregistre des
// captures des deux écrans (format PPM, empilés comme sur la console).
//
//   make -C tools/sim && ./tools/sim/dsishop_sim out_dir

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../../source/app.h"
#include "../../source/games.h"
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

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    // Dossier de jeux factice (vrais fichiers : la liste et la
    // désinstallation passent par games.c, comme sur la console)
    snprintf(sim_games_dir, sizeof(sim_games_dir), "%s/roms", dir);
    mkdir(sim_games_dir, 0777);
    fake_rom("Pong.nds", 131072);
    fake_rom("Tetris DS Remix.dsi", 4718592);
    fake_rom("Zelda Homebrew.nds", 9437184);
    games_record(sim_games_dir, "Pong.nds", "github.com/auteur/pong", "v1.0.3");
    games_record(sim_games_dir, "Zelda Homebrew.nds", "github.com/x/zelda", "v0.9");

    gfx_init();
    app_init(net_init());
    frames(2);
    save(dir, "01_menu");

    // Wi-Fi -> réseaux enregistrés
    tap(128, 147);
    frames(2);
    tap(128, 47);
    frames(120);
    press(BTN_B);
    frames(2);

    // Remote : QR code, puis un téléphone se connecte
    tap(190, 78);
    frames(25);
    save(dir, "02_remote_qr");
    remote_sim_pair("Alice", "alice@example.com");
    frames(3);
    save(dir, "03_remote_connecte");
    press(BTN_B);
    frames(2);

    // Installer un jeu
    tap(66, 78);
    frames(2);
    save(dir, "04_installer");
    tap(128, 113);
    frames(90);
    save(dir, "05_telechargement");
    frames(110);
    save(dir, "06_installe");
    tap(128, 108 + 36 + 15);   // Plus tard
    frames(2);

    // Onglet "Mes jeux" puis désinstallation
    tap(188, 36);
    frames(2);
    save(dir, "07_mes_jeux");
    tap(100, 52 + 16);
    frames(2);
    save(dir, "08_desinstaller");
    tap(128, 119);
    frames(3);
    save(dir, "09_apres_desinstallation");

    press(BTN_B);
    frames(2);
    tap(210, 147);   // Plus…
    frames(2);
    save(dir, "10_plus");
    return 0;
}
