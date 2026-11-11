// Jeux installés dans le dossier des ROM :
// - liste, désinstallation ;
// - registre (.dsishop.tsv) : d'où vient chaque jeu, sa version, l'adresse exacte
//   du téléchargement et les infos HTTP (ETag, date) pour détecter les mises à jour ;
// - copie de la version précédente (.dsishop_old/) pour revenir en arrière ;
// - fichiers de sauvegarde (.sav...) à côté de la ROM ou dans saves/.
// Portable (stdio + dirent) : testé sur PC dans tools/test.
#ifndef GAMES_H__
#define GAMES_H__

#include <stdbool.h>
#include <stdint.h>

#define GAMES_MAX 128
#define GAMES_MAX_SAVES 16

typedef struct {
    char file[128];     // Nom du fichier (ex. "Pong.nds")
    uint64_t size;
    char source[256];   // Lien saisi à l'installation (projet GitHub ou URL)
    char version[64];   // Tag de la release (ou vide)
    char url[512];      // Adresse exacte du fichier téléchargé
    char etag[80];      // En-tête HTTP ETag du téléchargement
    char modified[40];  // En-tête HTTP Last-Modified du téléchargement
} game_t;

// Liste les .nds / .dsi du dossier, triés par nom. Renvoie leur nombre.
int games_scan(const char *dir, game_t *out, int max);

// Nom acceptable : pas de dossier, pas de "..", extension .nds ou .dsi
bool games_valid_name(const char *file);

// Vrai si DSi Shop sait d'où vient le jeu (donc peut le mettre à jour)
bool games_is_tracked(const game_t *g);

// Supprime le jeu, sa copie de sauvegarde (.dsishop_old) et son entrée dans le
// registre. Les fichiers de sauvegarde (.sav) sont gardés.
bool games_uninstall(const char *dir, const char *file);

// Lit l'entrée du registre d'un jeu (false si absente)
bool games_get_record(const char *dir, const char *file, game_t *out);

// Enregistre (ou remplace) l'entrée du registre pour g->file
bool games_record(const char *dir, const game_t *g);

// Version précédente (gardée lors d'une mise à jour)
bool games_backup(const char *dir, const char *file);      // Déplace la ROM actuelle
bool games_has_backup(const char *dir, const char *file);
bool games_restore_backup(const char *dir, const char *file); // Échange les deux

// Sauvegardes du jeu : <nom>.sav, .sav1-.sav9, .pub, .prv, .bak, à côté de la
// ROM ou dans le sous-dossier saves/ (réglage par défaut de TWiLight Menu++).
// Remplit paths (si non NULL) et renvoie le nombre de fichiers trouvés.
int games_find_saves(const char *dir, const char *file,
                     char paths[][320], int max);
// Supprime ces fichiers. Renvoie le nombre de fichiers supprimés.
int games_delete_saves(const char *dir, const char *file);

#endif // GAMES_H__
