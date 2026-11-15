// Recherche de tous les jeux (.nds / .dsi) présents sur la carte SD, dans tous
// les dossiers. Se fait par petites étapes (quelques dizaines d'entrées par
// image) : l'interface reste fluide pendant l'analyse, sans tâche en arrière-plan.
//
// Les dossiers du système (_nds, title, private, DCIM, luma, 3ds...) et les
// dossiers cachés (nom commençant par "." ou "$") sont ignorés.
// Portable (dirent) : testé sur PC dans tools/test.

#ifndef SDSCAN_H__
#define SDSCAN_H__

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDSCAN_MAX   256   // Jeux gardés (les suivants sont ignorés : truncated)
#define SDSCAN_DEPTH 8     // Niveaux de dossiers explorés
#define SDSCAN_PATH  200

typedef struct {
    char path[SDSCAN_PATH];   // Chemin complet, ex. "sd:/jeux/Pong.nds"
    uint64_t size;
} sdgame_t;

typedef struct {
    sdgame_t games[SDSCAN_MAX];
    int count;
    bool done;          // Analyse terminée (liste triée par nom)
    bool truncated;     // Il y avait plus de SDSCAN_MAX jeux
    int dirs;           // Dossiers parcourus jusqu'ici
    int depth;
    DIR *dir[SDSCAN_DEPTH];
    size_t plen[SDSCAN_DEPTH];
    char path[SDSCAN_PATH];
} sdscan_t;

// Démarre l'analyse à partir de root ("sd:/"). Les résultats d'une analyse
// précédente sont effacés.
void sdscan_start(sdscan_t *s, const char *root);
// Traite jusqu'à `budget` entrées de dossier. Renvoie true quand c'est fini.
bool sdscan_step(sdscan_t *s, int budget);
// Abandonne l'analyse en cours (referme les dossiers ouverts)
void sdscan_cancel(sdscan_t *s);

// Nom du fichier (sans le dossier)
const char *sdgame_name(const sdgame_t *g);
// Dossier du fichier, sans "/" final ("sd:/jeux", "sd:" pour la racine de la carte)
void sdgame_folder(const sdgame_t *g, char *out, size_t size);

#endif // SDSCAN_H__
