// Protocole texte entre la DSi et le serveur Remote (voir remote-server/).
// Une info par ligne, champs séparés par des tabulations :
//   owner \t <nom> \t <email>          (ou "owner \t -" si pas associée)
//   pair  \t <code> \t <url> \t <durée en s>
//   device\t <id> \t <jeton>
//   cmd   \t <id> \t <type> \t <argument>
// La DSi envoie aussi, pour chaque jeu :
//   game  \t <fichier> \t <taille> \t <source> \t <version> \t <url>
//   error \t <message>
// Portable : testé sur PC dans tools/test.
#ifndef REMOTE_PROTO_H__
#define REMOTE_PROTO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "games.h"

typedef enum {
    RCMD_UNKNOWN,
    RCMD_INSTALL,    // arg = lien
    RCMD_UNINSTALL,  // arg = nom du fichier
    RCMD_UPDATE,     // arg = nom du fichier
    RCMD_CANCEL,
    RCMD_REBOOT,
    RCMD_REFRESH,
} rcmd_type_t;

typedef struct {
    char id[16];
    rcmd_type_t type;
    char arg[320];
} rcmd_t;

#define RREPLY_MAX_CMDS 8

typedef struct {
    bool owner_known;     // Une ligne "owner" était présente
    bool has_owner;
    char owner_name[64];
    char owner_email[96];

    bool has_pair;
    char pair_code[16];
    char pair_url[192];
    int pair_ttl;

    bool has_device;
    char device_id[24];
    char device_token[72];

    char error[128];

    int ncmds;
    rcmd_t cmds[RREPLY_MAX_CMDS];
} rreply_t;

void remote_parse_reply(const char *text, rreply_t *out);

// Statut envoyé au serveur
typedef struct {
    const char *state;     // idle, resolving, downloading, extracting...
    const char *message;
    const char *file;
    const char *release;
    const char *version;   // Version de DSi Shop
    uint64_t now, total;   // Progression
    uint64_t free_bytes;   // Espace libre sur la SD (0 = inconnu)
} rstatus_t;

typedef struct {
    char id[16];
    bool ok;
    char message[128];
} rack_t;

// Construit le corps du POST /api/device/status. Renvoie sa longueur.
size_t remote_build_status(char *buf, size_t cap, const rstatus_t *st,
                           const rack_t *acks, int nacks,
                           const game_t *games, int ngames);

#endif // REMOTE_PROTO_H__
