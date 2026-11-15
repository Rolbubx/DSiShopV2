// Remote : pilotage de la DSi depuis un téléphone, via le serveur relais
// (remote-server/). La DSi affiche un QR code ; on le scanne, on se connecte
// avec Google, et on peut ensuite installer / désinstaller / redémarrer.
#ifndef REMOTE_H__
#define REMOTE_H__

#include <stdbool.h>

typedef enum {
    REMOTE_OFF,          // Pas démarré
    REMOTE_NO_SERVER,    // Aucun serveur configuré
    REMOTE_WAIT_WIFI,    // En attente du Wi-Fi
    REMOTE_CONNECTING,   // Enregistrement / connexion au serveur
    REMOTE_WAIT_PAIR,    // QR code affiché, personne n'est associé
    REMOTE_PAIRED,       // Associée à un compte Google
    REMOTE_ERROR,        // Erreur temporaire (nouvel essai automatique)
} remote_state_t;

typedef enum {
    REMOTE_EV_NONE,
    REMOTE_EV_INSTALL_STARTED, // Une installation a été lancée à distance
    REMOTE_EV_GAMES_CHANGED,   // Un jeu a été désinstallé à distance
    REMOTE_EV_REBOOT,          // Redémarrage demandé
    REMOTE_EV_PAIRED,          // Un compte vient d'être associé
} remote_event_t;

// Charge la configuration (sd:/_dsishop/). À appeler après installer_init().
void remote_init(void);
// Démarre le Remote en arrière-plan (sans effet s'il tourne déjà)
void remote_start(void);
// Demande un nouveau code d'appairage (QR code)
void remote_request_pair_code(void);
// Oublie l'association avec le serveur (un nouveau QR sera généré)
void remote_forget(void);
// Vrai si la DSi a déjà été enregistrée (on relance le Remote au démarrage)
bool remote_has_device(void);

remote_state_t remote_state(void);
const char *remote_status_text(void);
const char *remote_server(void);
const char *remote_pair_url(void);    // "" si pas de code valide
const char *remote_pair_code(void);
const char *remote_owner_name(void);
const char *remote_owner_email(void);

// Événements pour l'interface (à lire à chaque frame)
remote_event_t remote_poll_event(void);

#endif // REMOTE_H__
