// Liaison entre le Remote local (lremote.c) et la DSi : installeur, jeux,
// redémarrage, aléatoire. Démarre / arrête le serveur selon le Wi-Fi.
#ifndef LREMOTE_GLUE_H__
#define LREMOTE_GLUE_H__

#include <stdbool.h>
#include <stddef.h>

#include "remote.h"

// À appeler après installer_init()
void lremote_glue_init(void);
// À chaque frame : démarre/arrête le serveur, traite les requêtes
void lremote_glue_update(unsigned int frame);
// Ajoute de l'entropie (entrées, compteurs...) à la réserve aléatoire
void lremote_glue_entropy(const void *data, size_t len);
// Événements pour l'interface (mêmes valeurs que le Remote en ligne)
remote_event_t lremote_glue_poll_event(void);
// Adresse à afficher dans le QR code ("" si pas de Wi-Fi)
const char *lremote_glue_url(void);
// Le serveur n'a pas pu démarrer (port occupé...)
bool lremote_glue_failed(void);

#endif // LREMOTE_GLUE_H__
