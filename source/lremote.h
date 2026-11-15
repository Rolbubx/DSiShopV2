// Remote local : la DSi sert elle-même une page web sur le Wi-Fi de la maison.
// On s'y connecte depuis un téléphone avec un identifiant et un mot de passe
// choisis sur la DSi (pas de compte Google, pas de serveur externe).
//
// Ce module est portable (testé sur PC dans tools/test) :
// - lremote.c     : identifiants, sessions, requêtes HTTP, réponses JSON
// - lremote_net.c : serveur TCP non bloquant (sockets BSD : lwIP sur DS)
// La DSi fournit ses données et actions via lremote_backend_t.
#ifndef LREMOTE_H__
#define LREMOTE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "games.h"
#include "remote_proto.h"

#define LREMOTE_PORT 80

#define LREMOTE_USER_MIN 3
#define LREMOTE_USER_MAX 32
#define LREMOTE_PASS_MIN 6
#define LREMOTE_PASS_MAX 64

typedef struct {
    // Jeux installés et état de l'installeur
    int (*list_games)(game_t *out, int max);
    void (*fill_status)(rstatus_t *st);
    // "available", "uptodate", "checking" ou ""
    const char *(*update_status)(const char *file);
    // Exécute une commande déjà validée. Renvoie false + message si refusée.
    bool (*command)(const char *type, const char *arg, char *msg, size_t msg_size);
    // Octets aléatoires (sel du mot de passe, jetons de session)
    void (*random)(void *buf, size_t len);
} lremote_backend_t;

// --- Configuration (fichier texte : identifiant, sel, empreinte) ------------

void lremote_init(const lremote_backend_t *backend, const char *config_path);
bool lremote_configured(void);
const char *lremote_user(void);
// Vérifie les longueurs, tire un sel, enregistre. Ferme les sessions ouvertes.
bool lremote_set_credentials(const char *user, const char *pass, char *err,
                             size_t err_size);
void lremote_clear_credentials(void);
bool lremote_enabled(void);
void lremote_set_enabled(bool enabled);

// --- HTTP ----------------------------------------------------------------------

// Longueur de la requête si elle est complète dans buf, 0 s'il manque des
// données, -1 si elle est invalide ou trop grosse.
long lremote_request_length(const char *buf, size_t len);

// Traite une requête complète. Renvoie la réponse HTTP (à libérer avec free)
// et sa longueur dans out_len.
char *lremote_handle(const char *req, size_t len, size_t *out_len);

// Nombre de sessions ouvertes (téléphones connectés)
int lremote_sessions(void);

// --- Serveur TCP (lremote_net.c) -----------------------------------------------

bool lremote_net_start(int port);
void lremote_net_stop(void);
bool lremote_net_running(void);
// À appeler à chaque frame : accepte, lit et répond sans jamais bloquer
void lremote_net_poll(void);

#endif // LREMOTE_H__
