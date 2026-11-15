// Point de reprise d'un téléchargement de jeu : si la console s'éteint ou que le
// Wi-Fi coupe, l'installation repart du dernier point sûr au lieu de tout
// recommencer. Ce fichier (.dsishop_resume, dans le dossier d'installation) décrit le
// téléchargement en cours ; les octets déjà reçus sont dans les morceaux .000, .001...
// Portable (stdio) : testé sur PC dans tools/test.

#ifndef DLRESUME_H__
#define DLRESUME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char link[256];     // Lien saisi par l'utilisateur
    char url[512];      // Adresse exacte du fichier
    char name[128];     // Nom du fichier
    char release[64];   // Version (tag de release), "" si inconnue
    char etag[80];      // ETag du serveur, "" si inconnu
    uint64_t total;     // Taille totale (0 : inconnue)
    uint64_t done;      // Octets déjà écrits ET synchronisés sur la carte
    bool complete;      // Le téléchargement est fini (reste l'extraction)
} dlresume_t;

bool dlresume_load(const char *path, dlresume_t *r);
// Écriture atomique (fichier temporaire puis renommage)
bool dlresume_save(const char *path, const dlresume_t *r);
void dlresume_clear(const char *path);

// Le point de reprise décrit-il bien ce téléchargement ? (même lien, même fichier,
// même version, même taille quand elle est connue)
bool dlresume_matches(const dlresume_t *r, const char *link, const char *url,
                      const char *release, const char *name, uint64_t size_hint);

#endif // DLRESUME_H__
