// Couche réseau : Wi-Fi (DSWiFi, WPA2 en mode DSi) + client HTTP asynchrone
#ifndef NET_H__
#define NET_H__

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    NET_OFF,         // Wi-Fi non initialisé / indisponible
    NET_IDLE,        // Initialisé, pas connecté
    NET_SCANNING,    // Recherche de réseaux en cours
    NET_CONNECTING,  // Association / DHCP en cours
    NET_CONNECTED,   // Connecté avec une adresse IP
    NET_FAILED,      // La dernière connexion a échoué
} net_state_t;

typedef enum {
    NET_SEC_OPEN,
    NET_SEC_WEP,
    NET_SEC_WPA,
    NET_SEC_WPA2,
} net_security_t;

typedef struct {
    char ssid[33];
    int rssi;            // dBm
    int channel;
    net_security_t security;
    bool saved;          // Configuré dans les paramètres Wi-Fi de la console
    bool compatible;     // Connectable par DSWiFi
} net_ap_t;

typedef struct {
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns1[16];
    char dns2[16];
} net_info_t;

// Initialise le Wi-Fi (sans se connecter). Renvoie false en cas d'échec.
bool net_init(void);
// À appeler à chaque frame
void net_update(void);

net_state_t net_state(void);
const char *net_state_text(void);   // Texte lisible de l'état courant
bool net_is_dsi_mode(void);         // Wi-Fi DSi (WPA2) ou DS (WEP/ouvert)
const char *net_ssid(void);         // SSID du réseau en cours / connecté
int net_signal_bars(void);          // 0..4
void net_get_info(net_info_t *info);

// Connexion aux réseaux enregistrés dans les paramètres de la console
void net_connect_saved(void);
// Recherche de réseaux
void net_scan_start(void);
void net_scan_stop(void);
int net_scan_count(void);
bool net_scan_get(int index, net_ap_t *out);
// Connexion à un réseau de la liste (password peut être NULL/"" si ouvert ou
// enregistré)
bool net_connect_ap(int index, const char *password);
void net_disconnect(void);

// ---------------------------------------------------------------------------
// HTTP (texte clair uniquement, pas de HTTPS)
// ---------------------------------------------------------------------------

typedef enum {
    HTTP_IDLE,
    HTTP_BUSY,
    HTTP_DONE,
    HTTP_ERROR,
} http_state_t;

// Lance un GET http://host/path en arrière-plan (une requête à la fois)
bool http_get_async(const char *host, const char *path);
http_state_t http_state(void);
const char *http_progress_text(void); // Étape en cours ou message d'erreur
int http_status_code(void);           // Code HTTP (200, 404...)
const char *http_body(void);          // Corps de la réponse (tronqué)
size_t http_bytes_received(void);     // Taille totale reçue (en-têtes inclus)

#endif // NET_H__
