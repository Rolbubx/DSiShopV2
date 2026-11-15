// Petites fonctions de la "pré-description" d'une installation (nom, auteur,
// taille...) et du mode détaillé. Portable (sans libnds) : testé sur PC dans
// tools/test.

#ifndef INSTINFO_H__
#define INSTINFO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Taille ("size": 12345) du fichier de la release dont l'adresse de
// téléchargement vient d'être lue : `end` est la fin de cette adresse dans le
// JSON `json`. 0 si elle n'est pas indiquée.
uint64_t instinfo_asset_size(const char *json, const char *end);

// "https://serveur/api/repos/auteur/projet/releases/latest" -> auteur, projet
bool instinfo_repo_of_api_url(const char *api_url, char *owner, size_t owner_size,
                              char *repo, size_t repo_size);

// "https://exemple.fr:8080/dossier/jeu.nds?x=1" -> "exemple.fr"
void instinfo_url_host(const char *url, char *out, size_t size);

// "52 Ko/s", "1,3 Mo/s" (vide si la vitesse est nulle)
void instinfo_format_speed(char *out, size_t size, uint64_t bytes_per_sec);

// "~ 45 s", "~ 2 min 10 s", "~ 1 h 05" (vide si la vitesse est nulle)
void instinfo_format_eta(char *out, size_t size, uint64_t remaining, uint64_t bytes_per_sec);

#endif // INSTINFO_H__
