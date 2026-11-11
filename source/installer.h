// Installation de jeux/homebrews depuis un lien GitHub ou une URL directe :
// téléchargement HTTPS (libcurl + Mbed TLS) découpé en morceaux de 1 Go (pour
// dépasser la limite de 4 Go du FAT32), extraction des .zip (ZIP64 compris),
// vérification de l'en-tête de la ROM et copie sur la carte SD.
#ifndef INSTALLER_H__
#define INSTALLER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    INST_IDLE,
    INST_RESOLVING,    // Interrogation de l'API GitHub
    INST_DOWNLOADING,  // Téléchargement du fichier
    INST_EXTRACTING,   // Extraction du .zip
    INST_VERIFYING,    // Vérification de la ROM
    INST_DONE,
    INST_ERROR,
} inst_state_t;

// Monte la carte SD et initialise libcurl. Renvoie false si pas de SD.
bool installer_init(void);
bool installer_storage_ok(void);
// Dossier où les jeux sont installés (ex. "sd:/roms/nds")
const char *installer_dir(void);

// Lance l'installation en arrière-plan. Formats acceptés :
//   github.com/auteur/projet            dernière release
//   github.com/auteur/projet/releases/tag/v1.0
//   auteur/projet
//   https://exemple.com/jeu.nds (ou .dsi, .zip)
bool installer_start(const char *link);
// Pareil, en précisant si la demande vient du Remote (pas de redémarrage auto)
bool installer_start_ex(const char *link, bool remote);
bool installer_was_remote(void);
void installer_cancel(void);

// Configure la vérification HTTPS d'un handle libcurl (CURL *)
void installer_setup_tls(void *curl);
// Espace libre sur la carte SD en octets (0 si inconnu)
uint64_t installer_free_space(void);

inst_state_t installer_state(void);
const char *installer_message(void);   // Étape en cours ou message d'erreur
const char *installer_file_name(void); // Nom du fichier installé / en cours
const char *installer_release(void);   // Tag de la release GitHub (ou "")
const char *installer_dest_path(void); // Chemin final sur la SD
uint64_t installer_progress_now(void);   // Octets traités (64 bits : > 4 Go)
uint64_t installer_progress_total(void); // Total (0 si inconnu)

#endif // INSTALLER_H__
