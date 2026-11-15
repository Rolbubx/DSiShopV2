// Mise à jour de TWiLight Menu++ depuis le serveur du catalogue.
//
// La release officielle est une archive .7z (LZMA2, dictionnaire de 16 Mo) :
// impossible à décompresser sur la console. Le serveur la découpe donc en
// petits paquets .zip indépendants, décrits par un manifeste :
//
//   twl1 <TAB> version <TAB> nombre de paquets <TAB> place nécessaire (octets)
//   nom.zip <TAB> taille <TAB> taille extraite <TAB> sha256 <TAB> condition
//   ...
//
// "condition" vaut "-" ou un chemin de la carte SD : le paquet n'est appliqué
// que si ce chemin existe déjà (fichiers réservés à hiyaCFW par exemple).
//
// Les paquets sont téléchargés, vérifiés (SHA-256) et extraits un par un. Le
// numéro du prochain paquet est noté sur la carte : après une coupure, la
// mise à jour reprend où elle s'était arrêtée. Le dernier paquet contient
// BOOT.NDS et version.txt.
//
// Portable (stdio) : testé sur PC dans tools/test.

#ifndef SYSUPDATE_H__
#define SYSUPDATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYSUPD_MAX_PACKS 48

typedef struct {
    char name[40];
    uint32_t size;       // Taille du .zip
    uint32_t unc_size;   // Taille une fois extrait
    char sha[65];        // SHA-256 du .zip, en hexadécimal
    char cond[128];      // Chemin qui doit exister ("" : toujours appliqué)
} sysupd_pack_t;

typedef struct {
    char tag[64];        // Version proposée, ex. "v27.25.1"
    int count;
    uint64_t need;       // Place libre nécessaire sur la carte
    sysupd_pack_t packs[SYSUPD_MAX_PACKS];
} sysupd_manifest_t;

typedef enum { SYSUPD_DOWNLOAD, SYSUPD_VERIFY, SYSUPD_EXTRACT } sysupd_phase_t;

typedef struct {
    // Télécharge "<serveur><path>" dans le fichier découpé dest_base.
    // Renvoie 0, ou -1 avec un message dans err.
    int (*fetch)(void *ud, const char *path, const char *dest_base, char *err,
                 size_t err_size);
    // Appelé régulièrement (done/total : octets du paquet en cours, sauf
    // pendant le téléchargement). Renvoyer non nul pour annuler.
    int (*progress)(void *ud, int pack, sysupd_phase_t phase, uint64_t done,
                    uint64_t total);
    void *ud;
} sysupd_io_t;

// Lit le manifeste (le texte est modifié). Renvoie false s'il est invalide.
bool sysupd_parse_manifest(char *text, sysupd_manifest_t *m);

// TWiLight Menu++ est-il installé sur cette carte ?
bool sysupd_present(const char *root);
// Version installée (lue dans version.txt), ex. "v27.25.1". false si inconnue.
bool sysupd_installed_version(const char *root, char *out, size_t size);
// Numéro du paquet où reprendre une mise à jour interrompue vers `tag` (0 sinon)
int sysupd_resume_index(const char *root, const char *tag);

// Applique la mise à jour. Renvoie 0, ou -1 avec un message dans err.
int sysupd_apply(const char *root, const sysupd_manifest_t *m, const sysupd_io_t *io,
                 char *err, size_t err_size);

#endif // SYSUPDATE_H__
