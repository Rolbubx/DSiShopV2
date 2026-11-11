// Simulateur PC : exécute app.c avec des entrées scriptées et enregistre des
// captures des deux écrans (format PPM, empilés comme sur la console).
//
//   make -C tools/sim && ./tools/sim/dsishop_sim out_dir

#include <stdio.h>
#include <string.h>

#include "../../source/app.h"
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

static void hold_touch(int x, int y)
{
    in.down = BTN_TOUCH; in.held = BTN_TOUCH; in.touching = true; in.tx = x; in.ty = y;
    frames(1);
    in.held = BTN_TOUCH; in.touching = true; in.tx = x; in.ty = y;
    gfx_present();
    net_update();
    app_frame(&in);
    gfx_present();
}

static void press(uint32_t key)
{
    in.down = key; in.repeat = key; in.held = key;
    frames(1);
    in.up = key;
    frames(1);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    gfx_init();
    app_init(net_init());
    frames(2);
    save(dir, "01_accueil");

    press(BTN_A);           // Affiche le focus
    press(BTN_DOWN);
    save(dir, "02_accueil_focus");

    tap(128, 49);           // Wi-Fi
    frames(2);
    save(dir, "03_wifi");

    tap(128, 49 + 38);      // Rechercher des réseaux
    frames(31);
    save(dir, "04_scan");

    tap(100, 28 + 34 + 15); // Freebox-Salon (WPA2, pas enregistré)
    frames(2);
    // Tape quelques touches au clavier virtuel : "azer"
    tap(5 + 12, 88 + 12);
    tap(5 + 25 + 12, 88 + 12);
    tap(5 + 50 + 12, 88 + 12);
    hold_touch(5 + 75 + 12, 88 + 12);
    save(dir, "05_mot_de_passe");
    in.up = BTN_TOUCH; frames(1);

    // Valide avec START (trop court : affiche un toast)
    press(BTN_START);
    frames(2);
    save(dir, "06_mot_de_passe_trop_court");

    // Annule et se connecte au réseau enregistré
    tap(178, 178);          // Touche "Annuler" du clavier
    frames(2);
    tap(100, 28 + 15);      // Livebox (enregistré)
    frames(20);
    save(dir, "07_connexion");
    frames(100);
    save(dir, "08_connecte");

    press(BTN_B);           // Retour accueil
    frames(2);
    tap(128, 49 + 38);      // Test Internet
    frames(2);
    tap(128, 95);           // Lancer
    frames(3);
    save(dir, "09_http");

    press(BTN_B);
    tap(70, 49 + 76);       // À propos
    frames(2);
    save(dir, "10_a_propos");
    return 0;
}
