// Faux réseau pour le simulateur PC (tools/sim)

#include <stdio.h>
#include <string.h>

#include "../../source/net.h"

static net_state_t state = NET_IDLE;
static int connect_timer;
static char ssid[33];

static const net_ap_t fake_aps[] = {
    { "Livebox-4F2A", -48, 6, NET_SEC_WPA2, true, true },
    { "Freebox-Salon", -61, 11, NET_SEC_WPA2, false, true },
    { "Wi-Fi Invités", -70, 1, NET_SEC_OPEN, false, true },
    { "SFR_8C10", -79, 6, NET_SEC_WPA, false, true },
    { "Vieux routeur", -86, 3, NET_SEC_WEP, false, true },
};

bool net_init(void) { return true; }

void installer_sim_tick(void);
void remote_sim_tick(void);

void net_update(void)
{
    installer_sim_tick();
    remote_sim_tick();
    if (state == NET_CONNECTING && ++connect_timer > 90)
        state = NET_CONNECTED;
}

net_state_t net_state(void) { return state; }

const char *net_state_text(void)
{
    switch (state)
    {
        case NET_IDLE: return "Déconnecté";
        case NET_SCANNING: return "Recherche de réseaux…";
        case NET_CONNECTING: return "Obtention de l'adresse IP…";
        case NET_CONNECTED: return "Connecté";
        case NET_FAILED: return "Échec de la connexion";
        default: return "Wi-Fi indisponible";
    }
}

bool net_is_dsi_mode(void) { return true; }
const char *net_ssid(void) { return ssid; }
int net_signal_bars(void) { return state == NET_CONNECTED ? 3 : 0; }

void net_get_info(net_info_t *info)
{
    strcpy(info->ip, "192.168.1.42");
    strcpy(info->gateway, "192.168.1.1");
    strcpy(info->netmask, "255.255.255.0");
    strcpy(info->dns1, "192.168.1.1");
    strcpy(info->dns2, "0.0.0.0");
}

bool net_has_saved_networks(void) { return true; }

void net_connect_saved(void)
{
    strcpy(ssid, "Livebox-4F2A");
    state = NET_CONNECTING;
    connect_timer = 0;
}

void net_scan_start(void) { state = NET_SCANNING; }
void net_scan_stop(void) { state = NET_IDLE; }
int net_scan_count(void) { return state == NET_SCANNING ? 5 : 0; }

bool net_scan_get(int index, net_ap_t *out)
{
    if (index < 0 || index >= net_scan_count())
        return false;
    *out = fake_aps[index];
    return true;
}

bool net_connect_ap(int index, const char *password)
{
    (void)password;
    strcpy(ssid, fake_aps[index].ssid);
    state = NET_CONNECTING;
    connect_timer = 0;
    return true;
}

void net_disconnect(void) { state = NET_IDLE; }

static http_state_t hstate = HTTP_IDLE;

bool http_get_async(const char *host, const char *path)
{
    (void)host; (void)path;
    hstate = HTTP_DONE;
    return true;
}

http_state_t http_state(void) { return hstate; }
const char *http_progress_text(void) { return "Terminé"; }
int http_status_code(void) { return 200; }
const char *http_body(void)
{
    return "<!doctype html>\n<html>\n<head>\n    <title>Example Domain</title>\n"
           "    <meta charset=\"utf-8\" />\n</head>\n<body>\n<div>\n"
           "    <h1>Example Domain</h1>\n    <p>This domain is for use in "
           "illustrative examples in documents.</p>\n</div>\n</body>\n</html>\n";
}
size_t http_bytes_received(void) { return 1591; }
