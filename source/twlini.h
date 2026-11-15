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

// ---------------------------------------------------------------------------
// Lancer un jeu : DSi Shop règle TWiLight Menu++ pour qu'il démarre ce jeu au
// prochain démarrage (réglage "lancer la dernière ROM"), puis la console redémarre.
// ---------------------------------------------------------------------------
//
// TWiLight lance les jeux de façons différentes selon leur type, qu'il reconnaît
// à l'en-tête de la ROM : on reprend ses règles pour écrire les bons réglages.

typedef enum {
    ROM_INVALID,          // Illisible, ou en-tête invalide
    ROM_COMMERCIAL,       // Jeu DS fait avec le SDK de Nintendo : nds-bootstrap
    ROM_HOMEBREW_MODERN,  // Homebrew qui lit la SD seul (DSi Shop...) : lancé directement
    ROM_HOMEBREW_OLD,     // Homebrew à l'ancienne (pilote DLDI) : nds-bootstrap (hb)
    ROM_DSIWARE,          // DSiWare : à lancer depuis TWiLight Menu++
} rom_kind_t;

rom_kind_t twlini_classify_rom(const char *path);

typedef enum {
    LAUNCH_OK,
    LAUNCH_NOT_SD,        // Le jeu n'est pas sur la carte SD de la console ("sd:/...")
    LAUNCH_BAD_ROM,
    LAUNCH_DSIWARE,
    LAUNCH_WRITE,         // Réglages de TWiLight Menu++ non modifiables
} twl_launch_err_t;

// Règle TWiLight Menu++ pour lancer rom_path ("sd:/dossier/Jeu.nds") au prochain
// démarrage. L'ancien réglage "lancer la dernière ROM" est noté dans
// <root>/_dsishop/prev_autorun (sans écraser une note plus ancienne) pour pouvoir
// le remettre. kind (peut être NULL) reçoit le type reconnu.
twl_launch_err_t twlini_launch_set(const char *root, const char *rom_path, rom_kind_t *kind);

// Remet le réglage "lancer la dernière ROM" d'avant. Renvoie true s'il y avait
// quelque chose à remettre (et que c'est fait).
bool twlini_launch_restore(const char *root);

#endif // TWLINI_H__
