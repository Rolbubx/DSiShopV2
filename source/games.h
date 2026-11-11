// Liste des jeux installés dans le dossier des ROM, désinstallation, et petit
// registre (.dsishop.tsv) qui retient d'où vient chaque jeu et sa version.
// Portable (stdio + dirent) : testé sur PC dans tools/test.
#ifndef GAMES_H__
#define GAMES_H__

#include <stdbool.h>
#include <stdint.h>

#define GAMES_MAX 128

typedef struct {
    char file[128];     // Nom du fichier (ex. "Pong.nds")
    uint64_t size;
    char source[256];   // Lien d'origine (vide si installé à la main)
    char version[64];   // Tag de la release (ou vide)
} game_t;

// Liste les .nds / .dsi du dossier, triés par nom. Renvoie leur nombre.
int games_scan(const char *dir, game_t *out, int max);

// Nom acceptable : pas de dossier, pas de "..", extension .nds ou .dsi
bool games_valid_name(const char *file);

// Supprime le jeu (et son entrée dans le registre)
bool games_uninstall(const char *dir, const char *file);

// Retient la source et la version d'un jeu installé par DSi Shop
bool games_record(const char *dir, const char *file, const char *source,
                  const char *version);

#endif // GAMES_H__
