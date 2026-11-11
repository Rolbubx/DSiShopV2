// Couche réseau : DSWiFi + client HTTP dans une cothread

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nds.h>
#include <dswifi9.h>

#include "net.h"

#define MAX_APS 32

static struct {
    net_state_t state;
    bool dsi_mode;
    int assoc_status;

    // Copie de la liste des réseaux trouvés
    Wifi_AccessPoint aps[MAX_APS];
    int ap_count;
    unsigned int scan_timer;

    char ssid[33];
    int rssi;            // RSSI du réseau choisi (0 = inconnu)
} net;

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

bool net_init(void)
{
    // En mode DSi, DSWiFi utilise le chipset Wi-Fi de la DSi (WPA2 supporté).
    // Sinon il bascule automatiquement en mode DS (réseaux ouverts / WEP).
    unsigned int flags = INIT_ONLY | WIFI_ATTEMPT_DSI_MODE;

    if (!Wifi_InitDefault(flags))
    {
        net.state = NET_OFF;
        return false;
    }

    net.dsi_mode = isDSiMode();
    net.state = NET_IDLE;
    net.assoc_status = ASSOCSTATUS_DISCONNECTED;
    Wifi_EnableWifi();
    return true;
}

static void refresh_scan_list(void)
{
    int count = Wifi_GetNumAP();
    if (count > MAX_APS)
        count = MAX_APS;

    int n = 0;
    for (int i = 0; i < count; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        if (ap.ssid_len == 0) // Réseaux cachés : ignorés
            continue;
        net.aps[n++] = ap;
    }

    // Tri par puissance du signal décroissante
    for (int i = 1; i < n; i++)
    {
        Wifi_AccessPoint tmp = net.aps[i];
        int j = i - 1;
        while (j >= 0 && net.aps[j].rssi < tmp.rssi)
        {
            net.aps[j + 1] = net.aps[j];
            j--;
        }
        net.aps[j + 1] = tmp;
    }

    net.ap_count = n;
}

void net_update(void)
{
    switch (net.state)
    {
        case NET_SCANNING:
            // Inutile de recopier la liste à chaque frame
            if ((net.scan_timer++ % 30) == 0)
                refresh_scan_list();
            break;

        case NET_CONNECTING:
        {
            int status = Wifi_AssocStatus();
            net.assoc_status = status;
            if (status == ASSOCSTATUS_ASSOCIATED)
                net.state = NET_CONNECTED;
            else if (status == ASSOCSTATUS_CANNOTCONNECT)
                net.state = NET_FAILED;
            break;
        }

        case NET_CONNECTED:
        {
            int status = Wifi_AssocStatus();
            net.assoc_status = status;
            // Connexion perdue
            if (status != ASSOCSTATUS_ASSOCIATED)
                net.state = (status == ASSOCSTATUS_CANNOTCONNECT) ? NET_FAILED
                                                                  : NET_CONNECTING;
            break;
        }

        default:
            break;
    }
}

net_state_t net_state(void)
{
    return net.state;
}

const char *net_state_text(void)
{
    switch (net.state)
    {
        case NET_OFF:
            return "Wi-Fi indisponible";
        case NET_IDLE:
            return "Déconnecté";
        case NET_SCANNING:
            return "Recherche de réseaux…";
        case NET_CONNECTING:
            switch (net.assoc_status)
            {
                case ASSOCSTATUS_SEARCHING:      return "Recherche du réseau…";
                case ASSOCSTATUS_AUTHENTICATING: return "Authentification…";
                case ASSOCSTATUS_ASSOCIATING:    return "Association…";
                case ASSOCSTATUS_ACQUIRINGDHCP:  return "Obtention de l'adresse IP…";
                default:                         return "Connexion…";
            }
        case NET_CONNECTED:
            return "Connecté";
        case NET_FAILED:
            return "Échec de la connexion";
    }
    return "";
}

bool net_is_dsi_mode(void)
{
    return net.dsi_mode;
}

const char *net_ssid(void)
{
    return net.ssid;
}

static int rssi_to_bars(int rssi)
{
    if (rssi == 0)
        return 0;

    if (net.dsi_mode) // dBm
    {
        if (rssi >= -55) return 4;
        if (rssi >= -65) return 3;
        if (rssi >= -75) return 2;
        return 1;
    }

    // Mode DS : valeur brute 0..255 (échelle approximative)
    if (rssi >= 40) return 4;
    if (rssi >= 30) return 3;
    if (rssi >= 20) return 2;
    return 1;
}

int net_signal_bars(void)
{
    if (net.state != NET_CONNECTED)
        return 0;
    if (net.rssi == 0)
        return 4; // Réseau enregistré : puissance inconnue
    return rssi_to_bars(net.rssi);
}

static void addr_to_str(struct in_addr addr, char *out)
{
    // inet_ntoa() utilise un buffer statique, on recopie tout de suite
    snprintf(out, 16, "%s", inet_ntoa(addr));
}

void net_get_info(net_info_t *info)
{
    struct in_addr gw = { 0 }, mask = { 0 }, dns1 = { 0 }, dns2 = { 0 };
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);

    addr_to_str(ip, info->ip);
    addr_to_str(gw, info->gateway);
    addr_to_str(mask, info->netmask);
    addr_to_str(dns1, info->dns1);
    addr_to_str(dns2, info->dns2);
}

void net_connect_saved(void)
{
    if (net.state == NET_OFF)
        return;

    snprintf(net.ssid, sizeof(net.ssid), "Réseau enregistré");
    net.rssi = 0;

    Wifi_SetIP(0, 0, 0, 0, 0); // DHCP
    Wifi_AutoConnect();
    net.assoc_status = ASSOCSTATUS_SEARCHING;
    net.state = NET_CONNECTING;
}

void net_scan_start(void)
{
    if (net.state == NET_OFF)
        return;

    if (net.state == NET_CONNECTED || net.state == NET_CONNECTING)
        Wifi_DisconnectAP();

    Wifi_ScanMode();
    net.ap_count = 0;
    net.scan_timer = 0;
    net.state = NET_SCANNING;
}

void net_scan_stop(void)
{
    if (net.state != NET_SCANNING)
        return;
    Wifi_IdleMode();
    net.state = NET_IDLE;
}

int net_scan_count(void)
{
    return net.ap_count;
}

bool net_scan_get(int index, net_ap_t *out)
{
    if (index < 0 || index >= net.ap_count)
        return false;

    const Wifi_AccessPoint *ap = &net.aps[index];

    snprintf(out->ssid, sizeof(out->ssid), "%s", ap->ssid);
    out->rssi = ap->rssi;
    out->channel = ap->channel;
    out->saved = (ap->flags & WFLAG_APDATA_CONFIG_IN_WFC) != 0;
    out->compatible = (ap->flags & WFLAG_APDATA_COMPATIBLE) != 0;

    switch (ap->security_type)
    {
        case AP_SECURITY_OPEN: out->security = NET_SEC_OPEN; break;
        case AP_SECURITY_WEP:  out->security = NET_SEC_WEP;  break;
        case AP_SECURITY_WPA:  out->security = NET_SEC_WPA;  break;
        default:               out->security = NET_SEC_WPA2; break;
    }

    return true;
}

bool net_connect_ap(int index, const char *password)
{
    if (index < 0 || index >= net.ap_count)
        return false;

    // On garde une copie : la liste peut changer pendant la connexion
    Wifi_AccessPoint ap = net.aps[index];

    if (!(ap.flags & WFLAG_APDATA_COMPATIBLE))
        return false;

    snprintf(net.ssid, sizeof(net.ssid), "%s", ap.ssid);
    net.rssi = ap.rssi;

    Wifi_SetIP(0, 0, 0, 0, 0); // DHCP

    int ret;
    size_t len = password ? strlen(password) : 0;

    if (len == 0 && (ap.flags & WFLAG_APDATA_CONFIG_IN_WFC))
        ret = Wifi_ConnectWfcAP(&ap);
    else if (len == 0)
        ret = Wifi_ConnectSecureAP(&ap, NULL, 0);
    else
        ret = Wifi_ConnectSecureAP(&ap, password, len);

    if (ret != 0)
    {
        net.state = NET_FAILED;
        return false;
    }

    net.assoc_status = ASSOCSTATUS_SEARCHING;
    net.state = NET_CONNECTING;
    return true;
}

void net_disconnect(void)
{
    if (net.state == NET_OFF)
        return;
    Wifi_DisconnectAP();
    net.state = NET_IDLE;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

#define HTTP_BUFFER_SIZE (64 * 1024)
#define HTTP_TIMEOUT_FRAMES (15 * 60)

static struct {
    volatile http_state_t state;
    char host[128];
    char path[256];
    char progress[96];
    int status_code;
    size_t received;
    const char *body;
    cothread_t thread;
    bool thread_alive;
} http;

static char http_buffer[HTTP_BUFFER_SIZE + 1];

static int http_fail(const char *msg)
{
    snprintf(http.progress, sizeof(http.progress), "%s", msg);
    http.state = HTTP_ERROR;
    return -1;
}

static int http_thread(void *arg)
{
    (void)arg;

    snprintf(http.progress, sizeof(http.progress), "Résolution DNS de %s…", http.host);

    struct addrinfo hint = { 0 };
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    if (getaddrinfo(http.host, "80", &hint, &result) != 0 || result == NULL)
        return http_fail("Échec de la résolution DNS");

    snprintf(http.progress, sizeof(http.progress), "Connexion au serveur…");

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(result);
        return http_fail("Impossible de créer le socket");
    }

    if (connect(sock, result->ai_addr, result->ai_addrlen) != 0)
    {
        freeaddrinfo(result);
        close(sock);
        return http_fail("Connexion au serveur refusée");
    }
    freeaddrinfo(result);

    char request[512];
    int len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "User-Agent: DSiShop/0.1 (Nintendo DSi)\r\n"
                       "Accept: */*\r\n"
                       "Connection: close\r\n\r\n",
                       http.path, http.host);

    if (write(sock, request, len) != len)
    {
        close(sock);
        return http_fail("Échec de l'envoi de la requête");
    }

    snprintf(http.progress, sizeof(http.progress), "Réception de la réponse…");

    // Socket non bloquant : on rend la main à l'interface entre deux lectures
    int opt = 1;
    ioctl(sock, FIONBIO, (char *)&opt);

    size_t used = 0;
    long content_length = -1;
    size_t header_end = 0;
    int idle_frames = 0;

    while (1)
    {
        size_t space = HTTP_BUFFER_SIZE - used;
        if (space == 0)
            break; // Réponse tronquée

        ssize_t n = read(sock, http_buffer + used, space > 4096 ? 4096 : space);

        if (n > 0)
        {
            used += n;
            http.received = used;
            http_buffer[used] = '\0';
            idle_frames = 0;

            if (header_end == 0)
            {
                char *sep = strstr(http_buffer, "\r\n\r\n");
                if (sep)
                {
                    header_end = (sep - http_buffer) + 4;
                    char *cl = strstr(http_buffer, "Content-Length:");
                    if (cl == NULL)
                        cl = strstr(http_buffer, "content-length:");
                    if (cl && cl < sep)
                        content_length = strtol(cl + 15, NULL, 10);
                }
            }

            if (header_end && content_length >= 0 &&
                used >= header_end + (size_t)content_length)
                break;

            continue; // Il y a peut-être encore des données
        }

        if (n == 0)
            break; // Le serveur a fermé la connexion

        if (errno != EAGAIN && errno != EWOULDBLOCK)
            break;

        if (++idle_frames > HTTP_TIMEOUT_FRAMES)
        {
            close(sock);
            return http_fail("Délai d'attente dépassé");
        }

        cothread_yield_irq(IRQ_VBLANK);
    }

    shutdown(sock, SHUT_RDWR);
    close(sock);

    http_buffer[used] = '\0';

    if (sscanf(http_buffer, "HTTP/%*d.%*d %d", &http.status_code) != 1)
        return http_fail("Réponse HTTP invalide");

    http.body = header_end ? http_buffer + header_end : "";
    snprintf(http.progress, sizeof(http.progress), "Terminé");
    http.state = HTTP_DONE;
    return 0;
}

bool http_get_async(const char *host, const char *path)
{
    if (http.state == HTTP_BUSY)
        return false;

    // Libère la cothread de la requête précédente
    if (http.thread_alive)
    {
        if (!cothread_has_joined(http.thread))
            return false;
        cothread_delete(http.thread);
        http.thread_alive = false;
    }

    snprintf(http.host, sizeof(http.host), "%s", host);
    snprintf(http.path, sizeof(http.path), "%s", path);
    http.status_code = 0;
    http.received = 0;
    http.body = "";
    http_buffer[0] = '\0';
    http.state = HTTP_BUSY;
    snprintf(http.progress, sizeof(http.progress), "Démarrage…");

    http.thread = cothread_create(http_thread, NULL, 16 * 1024, 0);
    if (http.thread < 0)
    {
        http_fail("Impossible de créer la cothread");
        return false;
    }
    http.thread_alive = true;
    return true;
}

http_state_t http_state(void)
{
    return http.state;
}

const char *http_progress_text(void)
{
    return http.progress;
}

int http_status_code(void)
{
    return http.status_code;
}

const char *http_body(void)
{
    return http.body ? http.body : "";
}

size_t http_bytes_received(void)
{
    return http.received;
}
