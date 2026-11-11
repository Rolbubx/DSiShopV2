// DSi Shop — point d'entrée : init matérielle et boucle principale

#include <nds.h>
#include <dswifi9.h>

#include "app.h"
#include "gfx.h"
#include "input.h"
#include "installer.h"
#include "net.h"
#include "lremote_glue.h"
#include "remote.h"

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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Sur DSi on passe l'ARM9 à 134 MHz (rendu logiciel plus fluide)
    if (isDSiMode())
        setCpuClock(true);

    gfx_init();
    keysSetRepeat(20, 4);

    // Carte SD (pour installer les jeux) puis Wi-Fi
    installer_init();
    bool wifi_ok = net_init();
    remote_init();
    lremote_glue_init();
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
