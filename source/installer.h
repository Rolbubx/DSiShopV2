// Installation de jeux/homebrews depuis un lien GitHub ou une URL directe :
// téléchargement HTTPS (libcurl + Mbed TLS) découpé en morceaux de 1 Go (pour
// dépasser la limite de 4 Go du FAT32), extraction des .zip (ZIP64 compris),
// vérification de l'en-tête de la ROM et copie sur la carte SD.
#ifndef INSTALLER_H__
#define INSTALLER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "games.h"

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
// Petits réglages enregistrés sur la carte SD (dossier _dsishop)
bool installer_pref_get(const char *name, char *out, size_t size);
void installer_pref_set(const char *name, const char *value);

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

// ---------------------------------------------------------------------------
// Mises à jour
// ---------------------------------------------------------------------------
//
// Projet GitHub (ou fichier de release GitHub) : compare le tag de la dernière
// release avec celui installé. Lien direct : compare l'ETag / la date
// (requête HEAD), et à défaut le contenu du fichier téléchargé.
// La nouvelle version garde exactement le même nom de fichier, donc les
// sauvegardes (.sav) restent associées au jeu. L'ancienne version est gardée
// dans .dsishop_old pour pouvoir revenir en arrière.

typedef enum {
    UPD_UNKNOWN,      // Pas encore vérifié
    UPD_CHECKING,
    UPD_NOT_TRACKED,  // Installé à la main : on ne sait pas d'où il vient
    UPD_UP_TO_DATE,
    UPD_AVAILABLE,
    UPD_ERROR,
} upd_status_t;

// Met à jour un jeu (même déroulement qu'une installation, en arrière-plan).
// À la fin : état INST_DONE, et installer_up_to_date() dit s'il n'y avait rien
// de nouveau.
bool installer_update(const char *file, bool remote);
bool installer_last_was_update(void);
bool installer_up_to_date(void);

// Vérifie s'il existe des mises à jour pour tous ces jeux (en arrière-plan)
bool installer_check_updates(const game_t *games, int count);
bool installer_checking(void);
void installer_check_progress(int *done, int *total);
// Résultat pour un jeu (latest : version disponible, peut être vide)
upd_status_t installer_update_status(const char *file, const char **latest);

// ---------------------------------------------------------------------------
// Parcourir les projets GitHub d'un utilisateur
// ---------------------------------------------------------------------------

#define BROWSE_MAX 30

typedef enum { BROWSE_IDLE, BROWSE_LOADING, BROWSE_DONE, BROWSE_ERROR } browse_state_t;

typedef struct {
    char owner[40];  // Propriétaire du projet
    char name[64];   // Nom du projet (sans le propriétaire)
    char desc[80];   // Description (peut être vide)
    int stars;
} browse_repo_t;

// Liste (en arrière-plan) les projets publics d'un utilisateur GitHub
bool installer_browse_start(const char *user);
// Cherche un jeu par son nom (projets DS/DSi de GitHub, les plus étoilés d'abord)
bool installer_browse_search(const char *query);
bool installer_browse_is_search(void);
browse_state_t installer_browse_state(void);
const char *installer_browse_message(void);   // Message d'erreur
const char *installer_browse_user(void);
int installer_browse_count(void);
const browse_repo_t *installer_browse_get(int index);

// ---------------------------------------------------------------------------
// Mise à jour de TWiLight Menu++ (voir sysupdate.h)
// ---------------------------------------------------------------------------

typedef enum {
    TWL_IDLE,
    TWL_CHECKING,   // Demande la dernière version au serveur
    TWL_READY,      // Version connue : on peut lancer la mise à jour
    TWL_UPDATING,
    TWL_DONE,
    TWL_ERROR,
} twl_state_t;

// TWiLight Menu++ est-il installé sur la carte SD de la console ?
bool installer_twl_present(void);
// Interroge le serveur (en arrière-plan) : état TWL_READY ou TWL_ERROR
bool installer_twl_check(void);
// Télécharge et applique les paquets (en arrière-plan), après TWL_READY
bool installer_twl_update(void);
twl_state_t installer_twl_state(void);
const char *installer_twl_message(void);    // Message d'erreur
const char *installer_twl_installed(void);  // Version installée ("" si inconnue)
const char *installer_twl_latest(void);     // Version proposée par le serveur
// Paquet où reprendrait une mise à jour interrompue (0 : depuis le début)
int installer_twl_resume(void);
// Avancement : paquet en cours (à partir de 1), nombre de paquets, octets
void installer_twl_progress(int *pack, int *packs, uint64_t *now, uint64_t *total);

// ---------------------------------------------------------------------------
// Lancer DSi Shop au démarrage (réglage de TWiLight Menu++, voir twlini.h)
// ---------------------------------------------------------------------------

// Chemin de DSi Shop sur la carte (argv[0]), à donner au démarrage
void installer_set_self_path(const char *path);
// Détail de l'initialisation de la carte SD (pour diagnostiquer un échec)
const char *installer_storage_info(void);
// Possible seulement avec TWiLight Menu++ sur la SD et un chemin connu
bool installer_autoboot_available(void);
bool installer_autoboot_get(void);
bool installer_autoboot_set(bool on);

// ---------------------------------------------------------------------------
// Mise à jour de DSi Shop lui-même (dernière release GitHub du projet)
// ---------------------------------------------------------------------------

typedef enum {
    SELF_IDLE,
    SELF_CHECKING,   // Demande la dernière release
    SELF_READY,      // Version connue (voir installer_self_newer)
    SELF_UPDATING,   // Téléchargement et remplacement du fichier
    SELF_DONE,       // Remplacé : il faut redémarrer
    SELF_ERROR,
} self_state_t;

// Possible seulement si DSi Shop connaît son propre fichier sur la carte
bool installer_self_available(void);
bool installer_self_check(const char *current_version);
bool installer_self_update(void);
self_state_t installer_self_state(void);
const char *installer_self_message(void);  // Message d'erreur
const char *installer_self_latest(void);   // Version publiée, ex. "v0.6.0"
bool installer_self_newer(void);           // Plus récente que celle qui tourne

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
