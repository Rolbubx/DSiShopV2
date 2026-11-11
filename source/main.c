// DSi Shop — point d'entrée : init matérielle et boucle principale

#include <nds.h>

#include "app.h"
#include "gfx.h"
#include "input.h"
#include "net.h"

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

    bool wifi_ok = net_init();
    app_init(wifi_ok);

    input_t in = { 0 };

    while (1)
    {
        // Attend le VBlank en laissant tourner les autres cothreads (HTTP...)
        cothread_yield_irq(IRQ_VBLANK);

        // Envoie à l'écran ce qui a été dessiné à la frame précédente
        gfx_present();

        read_input(&in);
        net_update();
        app_frame(&in);

        if (app_should_quit())
            break;
    }

    // Retour au loader (TWiLight Menu++, hbmenu...) si possible
    return 0;
}
