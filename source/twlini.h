// Lecture / modification du fichier de réglages de TWiLight Menu++
// (_nds/TWiLightMenu/settings.ini), sans toucher aux autres lignes.
// Sert à lancer DSi Shop automatiquement au démarrage : TWiLight sait relancer
// tout seul la "dernière ROM lancée" (réglage AUTORUNGAME), sauf si on
// maintient B.
//
// Portable (stdio) : testé sur PC dans tools/test.

#ifndef TWLINI_H__
#define TWLINI_H__

#include <stdbool.h>
#include <stddef.h>

#define TWLINI_PATH "_nds/TWiLightMenu/settings.ini"

// Valeur de "clé = valeur" dans [section]. false si absente.
bool twlini_get(const char *path, const char *section, const char *key, char *out,
                size_t size);
// Modifie ou ajoute n clés dans [section] (créée si besoin). Le reste du
// fichier est recopié tel quel. Le fichier est créé s'il n'existe pas.
bool twlini_set(const char *path, const char *section, const char *const *keys,
                const char *const *values, int n);

// DSi Shop (chemin self, ex. "sd:/roms/nds/DSiShop.nds") est-il lancé
// automatiquement au démarrage de TWiLight Menu++ ?
bool twlini_autoboot_get(const char *root, const char *self);
// Active ou désactive ce lancement automatique
bool twlini_autoboot_set(const char *root, const char *self, bool on);

#endif // TWLINI_H__
