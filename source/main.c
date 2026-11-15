// DSi Shop — point d'entrée : init matérielle et boucle principale

#include <stdio.h>

#include <nds.h>
#include <dswifi9.h>

#include "app.h"
#include "font.h"
#include "gfx.h"
#include "input.h"
#include "installer.h"
#include "net.h"
#include "lremote_glue.h"
#include "remote.h"
#include "ui.h"

static void read_input(input_t *in)
{
    scanKeys();

    in->down = keysDown();
    in->held = keysHeld();
    in->up = keysUp();
    in->repeat = keysDownRepeat();
    in->touching = (in->held & KEY_TOUCH) != 0;

    // On garde la dernière position connue pour détecter les relâchements
    if (in->touching)
    {
        touchPosition touch;
        touchRead(&touch);
        in->tx = touch.px;
        in->ty = touch.py;
    }
}

// Diagnostic de démarrage (make DIAG=1) : chaque étape reste affichée assez
// longtemps pour être vue. Attente active : ne dépend pas des interruptions.
#ifdef BOOT_DIAG
static void diag_wait(int tenths)
{
    // swiDelay : 4 cycles par tour, à 33 MHz (x4 quand l'ARM9 est à 134 MHz)
    uint32_t loops = 838000 * (REG_SCFG_CLK & 1 && isDSiMode() ? 4 : 1);
    for (int i = 0; i < tenths; i++)
        swiDelay(loops);
}
static void diag_color(int r, int g, int b)
{
    BG_PALETTE[0] = RGB15(r, g, b);
    BG_PALETTE_SUB[0] = RGB15(r, g, b);
    diag_wait(20);
}
#else
#define diag_wait(tenths) ((void)0)
#define diag_color(r, g, b) ((void)0)
#endif

// Écran de démarrage : affiché tout de suite, avec l'étape en cours. Si la
// console reste bloquée pendant l'init (carte SD, Wi-Fi...), on voit où.
static void boot_step(int step, int total, const char *msg)
{
    canvas_t *top = gfx_canvas(SCREEN_TOP);
    canvas_t *bot = gfx_canvas(SCREEN_BOTTOM);

    gfx_clear(top, COL_BG);
    gfx_text_box(top, &font_title, 0, 70, SCREEN_W, ALIGN_CENTER, "DSi Shop",
                 COL_TEXT);
    gfx_text_box(top, &font_small, 0, 100, SCREEN_W, ALIGN_CENTER,
                 isDSiMode() ? "Mode DSi" : "Mode DS", COL_TEXT_DIM);

    gfx_clear(bot, COL_BG);
    char line[96];
    snprintf(line, sizeof(line), "Démarrage %d/%d : %s", step, total, msg);
    gfx_text_box(bot, &font_body, 8, 80, SCREEN_W - 16, ALIGN_CENTER, line,
                 COL_TEXT);
    gfx_fill_rect(bot, 48, 104, SCREEN_W - 96, 4, COL_PANEL);
    gfx_fill_rect(bot, 48, 104, (SCREEN_W - 96) * step / total, 4, COL_ACCENT);
    gfx_text_box(bot, &font_small, 8, 150, SCREEN_W - 16, ALIGN_CENTER,
                 "Bloqué sur le Wi-Fi ? Relance en maintenant B", COL_TEXT_OFF);

    // Affiche immédiatement (on n'est pas encore dans la boucle principale)
#ifndef BOOT_DIAG
    swiWaitForVBlank();
#endif
    gfx_present();
    diag_wait(15);
}

int main(int argc, char **argv)
{
    // Le lanceur (fondu blanc de TWiLight Menu++) peut laisser la luminosité
    // au blanc : sans ça, tout l'écran reste blanc sur une vraie DSi
    setBrightness(3, 0);

    // DIAGNOSTIC écran blanc : couleur de fond témoin à chaque étape, visible
    // sans gfx_init (mode 0, aucun fond actif => seule la couleur 0 s'affiche)
    REG_DISPCNT = MODE_0_2D;
    REG_DISPCNT_SUB = MODE_0_2D;
    BG_PALETTE[0] = RGB15(31, 0, 0);      // ROUGE : main() atteint
    BG_PALETTE_SUB[0] = RGB15(31, 0, 0);
    diag_color(31, 0, 0);

    // Plantage : affiche les registres au lieu d'un écran figé
    defaultExceptionHandler();

    // Sur DSi on passe l'ARM9 à 134 MHz (rendu logiciel plus fluide)
    if (isDSiMode())
        setCpuClock(true);

    BG_PALETTE[0] = RGB15(31, 31, 0);     // JAUNE : setCpuClock OK
    BG_PALETTE_SUB[0] = RGB15(31, 31, 0);
    diag_color(31, 31, 0);

    gfx_init();

    BG_PALETTE[0] = 0;                    // gfx_init OK : fond noir normal
    BG_PALETTE_SUB[0] = 0;
    keysSetRepeat(20, 4);

    // B maintenu au lancement : on démarre sans Wi-Fi (secours si l'init
    // du Wi-Fi bloque sur une console)
    scanKeys();
    bool skip_wifi = (keysHeld() & KEY_B) != 0;

    // Carte SD (pour installer les jeux) puis Wi-Fi
    boot_step(1, 4, "carte SD...");
    installer_init();
    // Notre propre chemin : sert à se lancer tout seul au démarrage
    installer_set_self_path(argc > 0 && argv ? argv[0] : NULL);
    boot_step(2, 4, skip_wifi ? "Wi-Fi désactivé (B)" : "Wi-Fi...");
    bool wifi_ok = skip_wifi ? false : net_init();
    boot_step(3, 4, "Remote...");
    remote_init();
    lremote_glue_init();
    boot_step(4, 4, "interface...");
    app_init(wifi_ok);

    // Connexion automatique aux réseaux enregistrés dans la console
    if (wifi_ok && net_has_saved_networks())
        net_connect_saved();

    // DSi déjà associée à un téléphone : le Remote démarre tout seul
    // (il attend que le Wi-Fi soit connecté)
    if (remote_has_device())
        remote_start();

    input_t in = { 0 };
    unsigned int frame = 0;

    while (1)
    {
        // Attend le VBlank en laissant tourner les autres cothreads (HTTP...)
        cothread_yield_irq(IRQ_VBLANK);

        // Envoie à l'écran ce qui a été dessiné à la frame précédente
        gfx_present();

        read_input(&in);
        net_update();

        // Remote local : démarre/arrête le serveur, répond aux requêtes. Les
        // entrées et le timer servent aussi de source d'aléatoire.
        uint32_t noise[3] = { in.held, (uint32_t)(in.tx << 16 | in.ty), REG_VCOUNT };
        lremote_glue_entropy(noise, sizeof(noise));
        lremote_glue_update(frame++);

        app_frame(&in);

        if (app_should_quit())
            break;

        if (app_should_reboot())
        {
            // Laisse le temps à l'écran de s'afficher, puis redémarre.
            // systemReboot() ne fonctionne que sur DSi ; sur DS on retourne
            // au menu de lancement, qui listera le nouveau jeu.
            if (wifi_ok)
                Wifi_DisconnectAP();
            if (isDSiMode())
            {
                systemReboot();
                while (1)
                    swiWaitForVBlank();
            }
            break;
        }
    }

    // Retour au loader (TWiLight Menu++, hbmenu...) si possible
    return 0;
}
