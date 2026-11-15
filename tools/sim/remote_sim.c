// Faux Remote pour le simulateur PC

#include <stdio.h>
#include <string.h>

#include "../../source/remote.h"

static remote_state_t state = REMOTE_OFF;
static int timer;
static char owner[64] = "", email[96] = "";
static remote_event_t pending = REMOTE_EV_NONE;

void remote_sim_tick(void);
void remote_sim_pair(const char *name, const char *mail);

void remote_init(void) {}
bool remote_has_device(void) { return false; }

void remote_start(void)
{
    if (state == REMOTE_OFF)
    {
        state = REMOTE_CONNECTING;
        timer = 0;
    }
}

void remote_sim_tick(void)
{
    if (state == REMOTE_CONNECTING && ++timer > 20)
        state = REMOTE_WAIT_PAIR;
}

void remote_sim_pair(const char *name, const char *mail)
{
    snprintf(owner, sizeof(owner), "%s", name);
    snprintf(email, sizeof(email), "%s", mail);
    state = REMOTE_PAIRED;
    pending = REMOTE_EV_PAIRED;
}

void remote_request_pair_code(void) {}
void remote_forget(void) { state = REMOTE_CONNECTING; timer = 0; owner[0] = 0; }

remote_state_t remote_state(void) { return state; }

const char *remote_status_text(void)
{
    switch (state)
    {
        case REMOTE_CONNECTING: return "Connexion au serveur…";
        case REMOTE_WAIT_PAIR: return "Scanne le QR code avec ton téléphone";
        case REMOTE_PAIRED: return "Connectée";
        default: return "";
    }
}

const char *remote_server(void) { return "https://dsishop.exemple.fr"; }
const char *remote_pair_url(void)
{
    return state >= REMOTE_WAIT_PAIR ? "https://dsishop.exemple.fr/p/K7QM2XPA" : "";
}
const char *remote_pair_code(void) { return state >= REMOTE_WAIT_PAIR ? "K7QM2XPA" : ""; }
const char *remote_owner_name(void) { return owner; }
const char *remote_owner_email(void) { return email; }

remote_event_t remote_poll_event(void)
{
    remote_event_t ev = pending;
    pending = REMOTE_EV_NONE;
    return ev;
}
