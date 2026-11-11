// Logique de l'application (écrans, navigation). Indépendant du matériel :
// n'utilise que gfx, ui, keyboard et net.
#ifndef APP_H__
#define APP_H__

#include "input.h"

#define APP_NAME    "DSi Shop"
#define APP_VERSION "0.2.0"

void app_init(bool wifi_ok);
// Une frame : logique + dessin des écrans qui ont changé
void app_frame(const input_t *in);
// Vrai quand l'utilisateur a demandé à quitter
bool app_should_quit(void);
// Vrai quand la console doit redémarrer (après une installation)
bool app_should_reboot(void);

#endif // APP_H__
