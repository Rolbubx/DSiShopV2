// Logique de l'application : écrans, navigation et rendu

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "games.h"
#include "gfx.h"
#include "installer.h"
#include "instinfo.h"
#include "keyboard.h"
#include "lremote.h"
#include "lremote_glue.h"
#include "net.h"
#include "sdscan.h"
#include "remote.h"
#include "thirdparty/qrcodegen.h"
#include "ui.h"

// Serveur utilisé pour le test HTTP (texte clair, pas de HTTPS)
#define TEST_HOST "example.com"
#define TEST_PATH "/"

typedef enum {
    SCREEN_HOME,          // Menu principal
    SCREEN_WIFI,
    SCREEN_SCAN,
    SCREEN_PASSWORD,
    SCREEN_HTTP,
    SCREEN_ABOUT,
    SCREEN_INSTALL,       // Saisie du lien + progression
    SCREEN_URL,           // Clavier pour taper le lien
    SCREEN_INSTALL_DONE,  // Installé : compte à rebours avant redémarrage
    SCREEN_GAME,          // Fiche d'un jeu : mise à jour, sauvegardes...
    SCREEN_CONFIRM,       // Confirmation (désinstaller, effacer les sauvegardes...)
    SCREEN_REMOTE,        // QR code pour piloter la DSi depuis un téléphone
    SCREEN_LR_EDIT,       // Clavier : identifiant / mot de passe du Remote local
    SCREEN_MORE,          // Test Internet, À propos, Quitter
    SCREEN_BROWSE_USER,   // Clavier : nom d'utilisateur GitHub à parcourir
    SCREEN_BROWSE,        // Liste des projets de cet utilisateur
    SCREEN_TWL,           // Mise à jour de TWiLight Menu++
    SCREEN_SETTINGS,      // Réglages : thème, Verbose, démarrage
    SCREEN_LAUNCH,        // Lancement d'un jeu : compte à rebours avant le redémarrage
} app_screen_t;

// Actions qui demandent une confirmation
typedef enum {
    CONFIRM_UNINSTALL,
    CONFIRM_DELETE_SAVES,
    CONFIRM_RESTORE,
} confirm_t;

// Onglets de l'écran Remote
enum { RTAB_LOCAL, RTAB_ONLINE };

// Modes de l'écran "Parcourir GitHub"
enum { BMODE_SEARCH, BMODE_USER };

// Onglets de l'écran "Installer un jeu" / "Gérer les jeux"
enum { TAB_INSTALL, TAB_GAMES, TAB_SD };

// Délai avant le redémarrage qui lance le jeu (laisse le temps d'annuler)
#define LAUNCH_DELAY_FRAMES (3 * 60)

// Taille max d'un QR code (version 10 : URL jusqu'à ~170 caractères)
#define QR_MAX_VERSION 10

// Délai avant le redémarrage automatique après une installation
#define REBOOT_DELAY_FRAMES (5 * 60)

static struct {
    app_screen_t screen;
    bool wifi_ok;
    bool quit;
    unsigned int frame;

    // Liste des réseaux
    int scan_scroll;
    int selected_ap;
    net_ap_t selected;

    char password[65];
    char url[256];

    // Parcourir les projets d'un utilisateur GitHub
    char browse_user[40];
    int browse_scroll;
    int browse_mode;      // BMODE_SEARCH ou BMODE_USER

    bool autoboot;        // DSi Shop se lance tout seul au démarrage

    bool reboot;
    unsigned int done_frame;
    inst_state_t last_inst_state;

    // Jeux installés
    int install_tab;
    game_t games[GAMES_MAX];
    int ngames;
    bool games_loaded;
    int games_scroll;
    // Gérer les jeux : tous les jeux (.nds / .dsi) de la carte SD, analysés par étapes
    sdscan_t sdscan;
    bool sd_started;      // Une analyse a été lancée
    bool sd_dirty;        // La carte a changé : à ré-analyser en revenant sur l'onglet
    int sd_scroll;
    char sel_dir[SDSCAN_PATH];  // Dossier du jeu affiché dans la fiche
    char sel_kind[48];          // Type reconnu (jeu DS, homebrew...)
    unsigned int launch_frame;  // Image où le lancement a été demandé
    bool sel_from_sd;           // La fiche vient de l'onglet "Toute la SD"
    game_t sel;           // Jeu affiché dans la fiche
    int sel_saves;        // Nombre de fichiers de sauvegarde trouvés
    bool sel_backup;      // Une version précédente est disponible
    confirm_t confirm;

    // Remote local : saisie des identifiants
    int remote_tab;
    char lr_user[LREMOTE_USER_MAX + 1];
    char lr_pass[LREMOTE_PASS_MAX + 1];
    int lr_field;         // 0 : identifiant, 1 : mot de passe
    bool lr_editing;      // Formulaire affiché même si déjà configuré

    // QR code du Remote (recalculé seulement si l'URL change)
    char qr_text[200];
    bool qr_ok;
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    // Pour ne redessiner l'écran du haut que si quelque chose a changé
    uint32_t top_signature;
    bool force_redraw;
} app;

static int reboot_seconds_left(void);

static void go(app_screen_t screen)
{
    if (screen == SCREEN_SETTINGS && app.screen != SCREEN_SETTINGS)
        app.autoboot = installer_autoboot_get();   // Relu sur la carte SD
    app.screen = screen;
    app.force_redraw = true;
    ui_reset_focus();
}

void app_init(bool wifi_ok)
{
    memset(&app, 0, sizeof(app));
    app.wifi_ok = wifi_ok;
    app.top_signature = 0xFFFFFFFF;
    snprintf(app.url, sizeof(app.url), "github.com/");
    app.last_inst_state = INST_IDLE;
    app.remote_tab = RTAB_LOCAL;

    // Thème choisi précédemment (clair / sombre)
    char theme[16];
    ui_set_theme(installer_pref_get("theme", theme, sizeof(theme)) &&
                 strcmp(theme, "light") == 0);

    go(SCREEN_HOME);
}

bool app_should_quit(void)
{
    return app.quit;
}

bool app_should_reboot(void)
{
    return app.reboot;
}

static const char *security_name(net_security_t s)
{
    switch (s)
    {
        case NET_SEC_OPEN: return "Ouvert";
        case NET_SEC_WEP:  return "WEP";
        case NET_SEC_WPA:  return "WPA";
        default:           return "WPA2";
    }
}

static int ap_bars(const net_ap_t *ap)
{
    int r = ap->rssi;
    if (r == 0)
        return 0;
    if (net_is_dsi_mode())
        return r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1;
    return r >= 40 ? 4 : r >= 30 ? 3 : r >= 20 ? 2 : 1;
}

static bool install_busy(void)
{
    inst_state_t s = installer_state();
    return s >= INST_RESOLVING && s <= INST_VERIFYING;
}

static bool twl_busy(void)
{
    twl_state_t s = installer_twl_state();
    return s == TWL_CHECKING || s == TWL_UPDATING;
}

static bool self_busy(void)
{
    self_state_t s = installer_self_state();
    return s == SELF_CHECKING || s == SELF_UPDATING;
}

static bool net_busy(void)
{
    net_state_t s = net_state();
    return s == NET_CONNECTING || s == NET_SCANNING || http_state() == HTTP_BUSY ||
           install_busy() || twl_busy() || self_busy();
}

// "4,2 Go", "1,4 Mo", "820 Ko"...
static void format_size(char *out, size_t size, uint64_t bytes)
{
    const uint64_t KB = 1024, MB = KB * 1024, GB = MB * 1024;
    if (bytes >= GB)
        snprintf(out, size, "%u,%u Go", (unsigned)(bytes / GB),
                 (unsigned)((bytes % GB) * 10 / GB));
    else if (bytes >= MB)
        snprintf(out, size, "%u,%u Mo", (unsigned)(bytes / MB),
                 (unsigned)((bytes % MB) * 10 / MB));
    else
        snprintf(out, size, "%u Ko", (unsigned)((bytes + KB - 1) / KB));
}

static void draw_progress_bar(canvas_t *c, rect_t r, uint64_t now, uint64_t total)
{
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, r.h / 2, COL_PANEL);
    if (total > 0)
    {
        int w = (int)((uint64_t)r.w * (now > total ? total : now) / total);
        if (w >= r.h)
            gfx_fill_round_rect(c, r.x, r.y, w, r.h, r.h / 2, COL_ACCENT);
    }
    else
    {
        // Taille inconnue : segment qui se déplace
        int seg = r.w / 4;
        int x = (int)((app.frame * 3) % (unsigned int)(r.w + seg)) - seg;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + seg > r.w ? r.w : x + seg;
        if (x1 - x0 >= r.h)
            gfx_fill_round_rect(c, r.x + x0, r.y, x1 - x0, r.h, r.h / 2, COL_ACCENT);
    }
}

// Petite coche dessinée à la main, centrée sur (cx, cy)
static void draw_check(canvas_t *c, int cx, int cy, uint16_t color)
{
    static const int8_t pts[][2] = {
        { -3, 0 }, { -2, 1 }, { -1, 2 }, { 0, 1 }, { 1, 0 }, { 2, -1 }, { 3, -2 },
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++)
        gfx_fill_rect(c, cx + pts[i][0], cy + pts[i][1], 2, 2, color);
}

// Champ de texte (lien, mot de passe), avec curseur clignotant optionnel
static void draw_text_field(canvas_t *c, rect_t f, const char *text,
                            const char *placeholder, bool cursor, bool focused)
{
    gfx_fill_round_rect(c, f.x, f.y, f.w, f.h, 5, focused ? COL_ACCENT : COL_BORDER);
    gfx_fill_round_rect(c, f.x + 1, f.y + 1, f.w - 2, f.h - 2, 4, COL_BG);

    int ty = f.y + (f.h - font_body.height) / 2;

    if (text[0] == '\0' && placeholder)
    {
        gfx_text_box(c, &font_body, f.x + 8, ty, f.w - 16, ALIGN_LEFT,
                     placeholder, COL_TEXT_OFF);
        return;
    }

    // Affiche la fin du texte s'il est trop long
    const char *shown = text;
    while (*shown && gfx_text_width(&font_body, shown) > f.w - 20)
        shown++;
    int end = gfx_text(c, &font_body, f.x + 8, ty, shown, COL_TEXT);
    if (cursor && ((app.frame / 30) & 1))
        gfx_vline(c, end + 1, ty + 1, font_body.height - 2, COL_ACCENT);
}

// ---------------------------------------------------------------------------
// Écran du haut
// ---------------------------------------------------------------------------

static void refresh_games(void)
{
    app.ngames = installer_storage_ok()
                     ? games_scan(installer_dir(), app.games, GAMES_MAX)
                     : 0;
    app.games_loaded = true;
    if (app.games_scroll > app.ngames)
        app.games_scroll = 0;
}

// Analyse de la carte SD (onglet "Toute la SD") : lancée ici, avancée par petites
// étapes à chaque image (voir sd_scan_tick)
static void sd_start_scan(void)
{
    if (!installer_storage_ok())
        return;
    char root[SDSCAN_PATH];
    snprintf(root, sizeof(root), "%s/", installer_root());
    sdscan_start(&app.sdscan, root);
    app.sd_started = true;
    app.sd_dirty = false;
    app.sd_scroll = 0;
}

// À l'ouverture de l'onglet : première analyse, ou la carte a changé depuis
static void sd_enter(void)
{
    if (!app.sd_started || app.sd_dirty)
        sd_start_scan();
}

static void sd_scan_tick(void)
{
    if (app.sd_started && !app.sdscan.done)
        sdscan_step(&app.sdscan, 40);
}

// Dessine un QR code (fond blanc + zone de silence), centré dans la boîte
static void draw_qr(canvas_t *c, rect_t box, const char *text)
{
    if (strcmp(text, app.qr_text) != 0)
    {
        uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
        snprintf(app.qr_text, sizeof(app.qr_text), "%s", text);
        app.qr_ok = qrcodegen_encodeText(text, tmp, app.qr, qrcodegen_Ecc_MEDIUM,
                                         qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                         qrcodegen_Mask_AUTO, true);
    }
    if (!app.qr_ok)
        return;

    const int quiet = 3; // Modules blancs autour (pour les lecteurs de QR)
    int n = qrcodegen_getSize(app.qr);
    int total = n + quiet * 2;
    int max = box.w < box.h ? box.w : box.h;
    int scale = max / total;
    if (scale < 1)
        scale = 1;
    int px = total * scale;
    int x0 = box.x + (box.w - px) / 2, y0 = box.y + (box.h - px) / 2;

    const uint16_t white = COLOR_RGB(255, 255, 255), black = COLOR_RGB(0, 0, 0);
    gfx_fill_round_rect(c, x0, y0, px, px, scale * 2, white);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(app.qr, x, y))
                gfx_fill_rect(c, x0 + (x + quiet) * scale, y0 + (y + quiet) * scale,
                              scale, scale, black);
}

// Icônes des tuiles du menu principal (dessinées à la main)
static void icon_download(canvas_t *c, int cx, int cy, uint16_t col)
{
    gfx_fill_rect(c, cx - 2, cy - 14, 5, 13, col);              // Tige
    for (int i = 0; i < 7; i++)                                  // Pointe
        gfx_hline(c, cx - 8 + i, cy - 2 + i, 17 - i * 2, col);
    gfx_fill_rect(c, cx - 13, cy + 4, 3, 9, col);                // Bac
    gfx_fill_rect(c, cx + 11, cy + 4, 3, 9, col);
    gfx_fill_rect(c, cx - 13, cy + 11, 27, 3, col);
}

// s : taille d'un module (3 : grande tuile, 2 : case du bas)
static void icon_qr(canvas_t *c, int cx, int cy, uint16_t col, uint16_t bg, int s)
{
    // Trois "yeux" de QR code et quelques modules
    const int eyes[3][2] = { { -5 * s, -5 * s }, { 2 * s, -5 * s }, { -5 * s, 2 * s } };
    for (int i = 0; i < 3; i++)
    {
        int x = cx + eyes[i][0], y = cy + eyes[i][1];
        gfx_fill_rect(c, x, y, 3 * s, 3 * s, col);
        gfx_fill_rect(c, x + s - 1, y + s - 1, s + 2, s + 2, bg);
        gfx_fill_rect(c, x + s, y + s, s, s, col);
    }
    static const int8_t dots[][2] = {
        { 1, 1 }, { 2, 3 }, { 4, 2 }, { 1, 5 }, { 3, 6 }, { 5, 4 }, { 6, 6 },
        { 0, 3 }, { 5, 0 }, { 6, 2 },
    };
    for (size_t i = 0; i < sizeof(dots) / sizeof(dots[0]); i++)
        gfx_fill_rect(c, cx + dots[i][0] * s - 1, cy + dots[i][1] * s - 1, s, s, col);
}

// Une cartouche de jeu
static void icon_games(canvas_t *c, int cx, int cy, uint16_t col, uint16_t bg)
{
    gfx_fill_round_rect(c, cx - 11, cy - 14, 23, 28, 3, col);   // Cartouche
    gfx_fill_rect(c, cx - 7, cy - 10, 15, 11, bg);              // Étiquette
    gfx_fill_rect(c, cx - 4, cy - 7, 9, 2, col);
    gfx_fill_rect(c, cx - 4, cy - 3, 6, 2, col);
    for (int i = 0; i < 4; i++)                                  // Contacts
        gfx_fill_rect(c, cx - 7 + i * 4, cy + 5, 2, 7, bg);
}

typedef enum { ICON_DOWNLOAD, ICON_QR, ICON_GAMES } icon_t;

// Grande tuile du menu principal
static bool ui_tile(canvas_t *c, rect_t r, const char *title, const char *sub,
                    icon_t icon, bool primary)
{
    bool pressed, focused;
    bool clicked = ui_hit(r, true, &pressed, &focused);

    uint16_t bg = primary ? COL_ACCENT_DK : COL_PANEL_HI;
    if (pressed)
        bg = gfx_blend(bg, COLOR_RGB(0, 0, 0), 5);

    if (focused)
    {
        gfx_round_rect(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 12, COL_ACCENT);
        gfx_round_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 11, COL_ACCENT);
    }
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, 10, bg);
    gfx_gradient_v(c, r.x + 10, r.y + 1, r.w - 20, 1,
                   gfx_blend(bg, COLOR_RGB(255, 255, 255), 4), bg);

    // Tuile bleue : texte blanc dans les deux thèmes
    uint16_t fg = primary ? COL_WHITE : COL_TEXT;

    int dy = pressed ? 1 : 0;
    int cx = r.x + r.w / 2, cy = r.y + 30 + dy;
    if (icon == ICON_DOWNLOAD)
        icon_download(c, cx, cy, fg);
    else if (icon == ICON_GAMES)
        icon_games(c, cx, cy, fg, bg);
    else
        icon_qr(c, cx, cy, fg, bg, 3);

    gfx_text_box(c, &font_body, r.x + 4, r.y + 54 + dy, r.w - 8, ALIGN_CENTER,
                 title, fg);
    gfx_text_box(c, &font_small, r.x + 4, r.y + 54 + font_body.height + dy, r.w - 8,
                 ALIGN_CENTER, sub, gfx_blend(bg, fg, 11));
    return clicked;
}

// Case du Remote, en bas du menu principal : icône, titre, état et pastille
static bool ui_remote_box(canvas_t *c, rect_t r, const char *title, const char *sub,
                          bool active)
{
    bool pressed, focused;
    bool clicked = ui_hit(r, true, &pressed, &focused);

    uint16_t bg = pressed ? COL_PANEL_DOWN : COL_PANEL_HI;
    if (focused)
    {
        gfx_round_rect(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 12, COL_ACCENT);
        gfx_round_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 11, COL_ACCENT);
    }
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, 10, bg);
    gfx_gradient_v(c, r.x + 10, r.y + 1, r.w - 20, 1,
                   gfx_blend(bg, COLOR_RGB(255, 255, 255), 4), bg);

    int dy = pressed ? 1 : 0;
    icon_qr(c, r.x + 24, r.y + r.h / 2 + dy, COL_TEXT, bg, 2);
    gfx_text_box(c, &font_body, r.x + 52, r.y + 3 + dy, r.w - 52 - 24, ALIGN_LEFT, title,
                 COL_TEXT);
    gfx_text_box(c, &font_small, r.x + 52, r.y + 3 + font_body.height + dy,
                 r.w - 52 - 24, ALIGN_LEFT, sub, COL_TEXT_DIM);
    gfx_fill_circle(c, r.x + r.w - 14, r.y + r.h / 2 + dy, 4, active ? COL_OK : COL_TEXT_OFF);
    return clicked;
}

// Barre d'onglets (renvoie l'onglet choisi)
static int ui_tabs(canvas_t *c, rect_t r, const char *const *labels, int count,
                   int current)
{
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, r.h / 2, COL_PANEL);
    int w = r.w / count;
    for (int i = 0; i < count; i++)
    {
        rect_t t = RECT(r.x + i * w, r.y, w, r.h);
        bool pressed, focused;
        if (ui_hit(t, true, &pressed, &focused))
            current = i;
        if (i == current)
            gfx_fill_round_rect(c, t.x + 1, t.y + 1, t.w - 2, t.h - 2, (t.h - 2) / 2,
                                COL_ACCENT_DK);
        if (focused)
            gfx_round_rect(c, t.x, t.y, t.w, t.h, t.h / 2, COL_ACCENT);
        gfx_text_box(c, &font_small, t.x + 4, t.y + (t.h - font_small.height) / 2,
                     t.w - 8, ALIGN_CENTER, labels[i],
                     i == current ? COL_WHITE : COL_TEXT_DIM);
    }
    return current;
}

// Texte de statut du Remote pour l'accueil
static void remote_summary(char *out, size_t size)
{
    if (lremote_enabled() && lremote_net_running())
    {
        int n = lremote_sessions();
        snprintf(out, size, "Remote local : %s%s", lremote_glue_url(),
                 n ? " (téléphone connecté)" : "");
        return;
    }
    switch (remote_state())
    {
        case REMOTE_PAIRED:
            snprintf(out, size, "Remote : connecté à %s", remote_owner_name());
            break;
        case REMOTE_NO_SERVER:
            snprintf(out, size, "Remote : pas de serveur configuré");
            break;
        case REMOTE_OFF:
            snprintf(out, size, "Remote : inactif");
            break;
        case REMOTE_WAIT_PAIR:
            snprintf(out, size, "Remote : en attente du scan");
            break;
        default:
            snprintf(out, size, "Remote : %s", remote_status_text());
            break;
    }
}

static void draw_clock(char *out, size_t size)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm)
        snprintf(out, size, "%02d:%02d", tm->tm_hour, tm->tm_min);
    else
        out[0] = '\0';
}

static void draw_top_header(canvas_t *c)
{
    char clock[8];
    draw_clock(clock, sizeof(clock));

    gfx_gradient_v(c, 0, 0, SCREEN_W, 22, COL_PANEL_HI, COL_PANEL);
    gfx_hline(c, 0, 22, SCREEN_W, COL_BORDER);

    gfx_text(c, &font_body, 8, (22 - font_body.height) / 2, APP_NAME, COL_TEXT);

    int x = SCREEN_W - 8 - gfx_text_width(&font_small, clock);
    gfx_text(c, &font_small, x, (22 - font_small.height) / 2, clock, COL_TEXT_DIM);

    int bars = net_signal_bars();
    uint16_t on = net_state() == NET_CONNECTED ? COL_OK : COL_TEXT_DIM;
    ui_wifi_icon(c, x - 24, 5, bars, on, COL_BORDER);
}

// Carte décrivant l'état de la connexion
static void draw_net_card(canvas_t *c, rect_t r)
{
    ui_panel(c, r);

    net_state_t s = net_state();
    uint16_t dot = COL_TEXT_OFF;
    if (s == NET_CONNECTED)
        dot = COL_OK;
    else if (s == NET_CONNECTING || s == NET_SCANNING)
        dot = COL_WARN;
    else if (s == NET_FAILED || s == NET_OFF)
        dot = COL_ERR;

    int x = r.x + 10, y = r.y + 8;

    if (net_busy() && s != NET_CONNECTED)
        ui_spinner(c, x + 4, y + font_body.height / 2, 5, app.frame, COL_WARN);
    else
        gfx_fill_circle(c, x + 4, y + font_body.height / 2, 4, dot);

    gfx_text_box(c, &font_body, x + 16, y, r.w - 36, ALIGN_LEFT,
                 net_state_text(), COL_TEXT);
    y += font_body.height + 4;

    char line[96];

    if (s == NET_CONNECTED || s == NET_CONNECTING)
    {
        snprintf(line, sizeof(line), "Réseau : %s", net_ssid());
        gfx_text_box(c, &font_small, x, y, r.w - 20, ALIGN_LEFT, line, COL_TEXT_DIM);
        y += font_small.height + 2;
    }

    if (s == NET_CONNECTED)
    {
        net_info_t info;
        net_get_info(&info);

        snprintf(line, sizeof(line), "IP : %s", info.ip);
        gfx_text(c, &font_small, x, y, line, COL_TEXT_DIM);
        snprintf(line, sizeof(line), "Passerelle : %s", info.gateway);
        gfx_text(c, &font_small, x + (r.w - 20) / 2, y, line, COL_TEXT_DIM);
        y += font_small.height + 2;

        snprintf(line, sizeof(line), "DNS : %s", info.dns1);
        gfx_text(c, &font_small, x, y, line, COL_TEXT_DIM);
        y += font_small.height + 2;
    }

    // Ligne du mode Wi-Fi seulement s'il reste de la place dans la carte
    if (app.wifi_ok && y + font_small.height <= r.y + r.h - 6)
    {
        const char *mode = net_is_dsi_mode() ? "Wi-Fi DSi : WPA2, WPA, WEP, ouvert"
                                             : "Wi-Fi DS : WEP et ouvert seulement";
        gfx_text_box(c, &font_small, x, r.y + r.h - font_small.height - 6,
                     r.w - 20, ALIGN_LEFT, mode, COL_TEXT_OFF);
    }
}

// Une ligne "Libellé  valeur" de la pré-description (renvoie sa hauteur)
#define FIELD_LABEL_W 64
static int draw_field(canvas_t *c, int x, int y, int w, const char *label,
                      const char *value, uint16_t value_col)
{
    gfx_text(c, &font_small, x, y, label, COL_TEXT_OFF);
    gfx_text_box(c, &font_small, x + FIELD_LABEL_W, y, w - FIELD_LABEL_W, ALIGN_LEFT,
                 value, value_col);
    return font_small.height + 1;
}

static void draw_top(canvas_t *c)
{
    gfx_gradient_v(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG2, COL_BG);
    draw_top_header(c);

    switch (app.screen)
    {
        case SCREEN_HOME:
        case SCREEN_MORE:
        case SCREEN_SETTINGS:
        {
            // Titre "hero"
            gfx_text_box(c, &font_title, 0, 30, SCREEN_W, ALIGN_CENTER,
                         "Bienvenue sur " APP_NAME, COL_TEXT);
            gfx_text_box(c, &font_small, 0, 30 + font_title.height + 1, SCREEN_W,
                         ALIGN_CENTER, "Installe des jeux depuis GitHub ou ton téléphone",
                         COL_TEXT_DIM);
            draw_net_card(c, RECT(12, 72, SCREEN_W - 24, 82));

            // Petit encart Remote
            rect_t r = RECT(12, 160, SCREEN_W - 24, 24);
            ui_panel(c, r);
            char line[128];
            remote_summary(line, sizeof(line));
            uint16_t dot = (remote_state() == REMOTE_PAIRED ||
                            (lremote_enabled() && lremote_net_running())) ? COL_OK : COL_TEXT_OFF;
            gfx_fill_circle(c, r.x + 12, r.y + r.h / 2, 3, dot);
            gfx_text_box(c, &font_small, r.x + 22, r.y + (r.h - font_small.height) / 2,
                         r.w - 30, ALIGN_LEFT, line, COL_TEXT_DIM);
            break;
        }

        case SCREEN_GAME:
        case SCREEN_CONFIRM:
        {
            const game_t *g = &app.sel;
            gfx_text(c, &font_title, 12, 30, "Fiche du jeu", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);
            int x = r.x + 8, w = r.w - 16, y = r.y + 6;
            int bottom = r.y + r.h - 4;
            char line[600], size[24];

            format_size(size, sizeof(size), g->size);
            snprintf(line, sizeof(line), "%s · %s", g->version[0] ? g->version : "version inconnue",
                     size);
            gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT);
            y += font_small.height + 2;

            if (games_is_tracked(g))
            {
                snprintf(line, sizeof(line), "Source : %s", g->source[0] ? g->source : g->url);
                gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
                y += font_small.height + 2;
                snprintf(line, sizeof(line), "Téléchargé depuis : %s",
                         g->url[0] ? g->url : "(non retenu)");
                y += gfx_text_wrap(c, &font_small, x, y, w, font_small.height * 3, line,
                                   COL_TEXT_DIM);
            }
            else
            {
                y += gfx_text_wrap(c, &font_small, x, y, w, font_small.height * 2,
                                   "Installé à la main : DSi Shop ne connaît pas "
                                   "son adresse de téléchargement.", COL_TEXT_DIM);
            }

            y += 2;
            snprintf(line, sizeof(line), "Type : %s", app.sel_kind);
            if (y + font_small.height <= bottom)
                gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
            y += font_small.height + 2;
            snprintf(line, sizeof(line), "Sauvegardes : %d fichier(s)%s", app.sel_saves,
                     app.sel_saves ? " (gardées pendant les mises à jour)" : "");
            if (y + font_small.height <= bottom)
                gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
            y += font_small.height + 2;
            snprintf(line, sizeof(line), "Version précédente : %s",
                     app.sel_backup ? "disponible" : "aucune");
            if (y + font_small.height <= bottom)
                gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
            break;
        }

        case SCREEN_LAUNCH:
        {
            gfx_text(c, &font_title, 12, 30, "Lancement du jeu", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);
            int x = r.x + 8, w = r.w - 16, y = r.y + 6;
            gfx_text_box(c, &font_body, x, y, w, ALIGN_LEFT, app.sel.file, COL_TEXT);
            y += font_body.height + 3;
            char line[200];
            snprintf(line, sizeof(line), "Type : %s", app.sel_kind);
            gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
            y += font_small.height + 8;
            gfx_text_wrap(c, &font_small, x, y, w, r.y + r.h - 4 - y,
                          "La console redémarre et TWiLight Menu++ lance ce jeu.\n"
                          "\n"
                          "Pour revenir au menu de TWiLight au prochain démarrage, "
                          "maintiens B. DSi Shop remettra alors ses réglages d'avant.",
                          COL_TEXT_DIM);
            break;
        }

        case SCREEN_REMOTE:
        case SCREEN_LR_EDIT:
        {
            gfx_text(c, &font_title, 12, 30,
                     app.remote_tab == RTAB_LOCAL ? "Remote local" : "Remote en ligne",
                     COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);
            int x = r.x + 8, y = r.y + 6;

            if (app.remote_tab == RTAB_LOCAL)
            {
                y += gfx_text_wrap(c, &font_small, x, y, r.w - 16, 80,
                                   "1. Connecte ton téléphone au même Wi-Fi que la DSi\n"
                                   "2. Scanne le QR code (ou tape l'adresse)\n"
                                   "3. Connecte-toi avec ton identifiant et ton mot de passe",
                                   COL_TEXT_DIM);
                y += 4;
                gfx_hline(c, x, y, r.w - 16, COL_BORDER);
                y += 6;
                gfx_text_wrap(c, &font_small, x, y, r.w - 16, r.y + r.h - y - 4,
                              "Seulement chez toi, DSi Shop ouvert. Pas de compte : "
                              "c'est la DSi qui sert la page.",
                              COL_TEXT_OFF);
                break;
            }

            if (remote_state() == REMOTE_NO_SERVER)
            {
                gfx_text_wrap(c, &font_small, x, y, r.w - 16, r.h - 12,
                              "Le Remote a besoin d'un serveur (dossier remote-server "
                              "du projet), hébergé où tu veux.\n"
                              "\n"
                              "Indique son adresse dans le fichier "
                              "_dsishop/server.txt à la racine de la carte SD, "
                              "par exemple :\n"
                              "https://dsishop.mondomaine.fr\n"
                              "\n"
                              "Ou compile DSi Shop avec REMOTE_SERVER=...",
                              COL_TEXT_DIM);
                break;
            }

            y += gfx_text_wrap(c, &font_small, x, y, r.w - 16, 80,
                               "1. Scanne le QR code avec l'appareil photo de ton "
                               "téléphone\n"
                               "2. Connecte-toi avec ton compte Google\n"
                               "3. Installe, cherche sur GitHub, désinstalle… "
                               "depuis ton téléphone",
                               COL_TEXT_DIM);
            y += 4;
            gfx_hline(c, x, y, r.w - 16, COL_BORDER);
            y += 6;

            remote_state_t rs = remote_state();
            uint16_t col = rs == REMOTE_PAIRED ? COL_OK
                         : rs == REMOTE_ERROR  ? COL_ERR
                                               : COL_WARN;
            char line[200];
            if (rs == REMOTE_PAIRED)
                snprintf(line, sizeof(line), "Connectée à %s", remote_owner_name());
            else
                snprintf(line, sizeof(line), "%s", remote_status_text());
            gfx_fill_circle(c, x + 4, y + font_body.height / 2, 4, col);
            gfx_text_box(c, &font_body, x + 14, y, r.w - 30, ALIGN_LEFT, line, COL_TEXT);
            y += font_body.height + 2;

            if (rs == REMOTE_PAIRED && remote_owner_email()[0])
            {
                gfx_text_box(c, &font_small, x + 14, y, r.w - 30, ALIGN_LEFT,
                             remote_owner_email(), COL_TEXT_DIM);
                y += font_small.height + 2;
            }

            const char *server = remote_server();
            const char *host = strstr(server, "://");
            snprintf(line, sizeof(line), "Serveur : %s", host ? host + 3 : server);
            gfx_text_box(c, &font_small, x, r.y + r.h - font_small.height - 5, r.w - 16,
                         ALIGN_LEFT, line, COL_TEXT_OFF);
            break;
        }



        case SCREEN_WIFI:
        case SCREEN_SCAN:
        case SCREEN_PASSWORD:
        {
            gfx_text(c, &font_title, 12, 32, "Wi-Fi", COL_TEXT);
            draw_net_card(c, RECT(12, 58, SCREEN_W - 24, 94));

            const char *hint = "";
            if (app.screen == SCREEN_SCAN)
                hint = "Touche un réseau pour t'y connecter";
            else if (app.screen == SCREEN_PASSWORD)
                hint = "B : effacer · SELECT : Maj · START : valider";
            else if (!net_is_dsi_mode() && app.wifi_ok)
                hint = "Astuce : lance l'appli en mode DSi pour le WPA2";
            gfx_text_box(c, &font_small, 12, 162, SCREEN_W - 24, ALIGN_CENTER,
                         hint, COL_TEXT_DIM);
            break;
        }

        case SCREEN_HTTP:
        {
            gfx_text(c, &font_title, 12, 30, "Test Internet", COL_TEXT);

            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);

            char line[128];
            http_state_t hs = http_state();
            int y = r.y + 6;

            if (hs == HTTP_IDLE)
            {
                snprintf(line, sizeof(line), "Requête prête : GET http://%s%s",
                         TEST_HOST, TEST_PATH);
                gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT,
                             line, COL_TEXT_DIM);
                break;
            }

            if (hs == HTTP_BUSY)
            {
                ui_spinner(c, r.x + 12, y + font_small.height / 2, 5, app.frame,
                           COL_ACCENT);
                snprintf(line, sizeof(line), "%s (%u octets)", http_progress_text(),
                         (unsigned)http_bytes_received());
                gfx_text_box(c, &font_small, r.x + 24, y, r.w - 32, ALIGN_LEFT,
                             line, COL_TEXT);
                break;
            }

            if (hs == HTTP_ERROR)
            {
                gfx_text_wrap(c, &font_body, r.x + 8, y, r.w - 16, r.h - 12,
                              http_progress_text(), COL_ERR);
                break;
            }

            // HTTP_DONE
            int code = http_status_code();
            snprintf(line, sizeof(line), "HTTP %d · %u octets reçus", code,
                     (unsigned)http_bytes_received());
            gfx_text(c, &font_body, r.x + 8, y,
                     line, (code >= 200 && code < 300) ? COL_OK : COL_WARN);
            y += font_body.height + 4;
            gfx_hline(c, r.x + 8, y, r.w - 16, COL_BORDER);
            y += 4;
            gfx_text_wrap(c, &font_small, r.x + 8, y, r.w - 16, r.y + r.h - y - 4,
                          http_body(), COL_TEXT_DIM);
            break;
        }

        case SCREEN_INSTALL:
        case SCREEN_URL:
        case SCREEN_BROWSE_USER:
        case SCREEN_BROWSE:
        {
            inst_state_t st = installer_state();
            bool manage = app.screen == SCREEN_INSTALL && app.install_tab == TAB_SD;
            gfx_text(c, &font_title, 12, 30, manage ? "Gérer les jeux" : "Installer un jeu",
                     COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);

            if (manage && (st == INST_IDLE || st == INST_DONE))
            {
                // Résumé de ce qui a été trouvé sur la carte SD
                const sdscan_t *s = &app.sdscan;
                bool scanning = app.sd_started && !s->done;
                int x = r.x + 8, w = r.w - 16, y = r.y + 6;
                char line[160], size[24];
                uint64_t total = 0;
                int in_shop = 0;
                for (int i = 0; i < s->count; i++)
                {
                    char folder[SDSCAN_PATH];
                    sdgame_folder(&s->games[i], folder, sizeof(folder));
                    total += s->games[i].size;
                    in_shop += strcmp(folder, installer_dir()) == 0;
                }

                if (!installer_storage_ok())
                    snprintf(line, sizeof(line), "Carte SD introuvable");
                else if (scanning)
                    snprintf(line, sizeof(line), "Analyse de la carte SD…");
                else
                    snprintf(line, sizeof(line), "%d jeu%s sur la carte SD", s->count,
                             s->count > 1 ? "x" : "");
                gfx_text_box(c, &font_body, x, y, w, ALIGN_LEFT, line, COL_TEXT);
                y += font_body.height + 4;

                if (installer_storage_ok())
                {
                    snprintf(line, sizeof(line), "%d dossier%s parcouru%s%s", s->dirs,
                             s->dirs > 1 ? "s" : "", s->dirs > 1 ? "s" : "",
                             scanning ? "…" : "");
                    gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
                    y += font_small.height + 1;
                    format_size(size, sizeof(size), total);
                    snprintf(line, sizeof(line), "Place prise par ces jeux : %s", size);
                    gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
                    y += font_small.height + 1;
                    snprintf(line, sizeof(line), "Dont dans le dossier de DSi Shop : %d", in_shop);
                    gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_TEXT_DIM);
                    y += font_small.height + 5;
                }
                if (s->truncated)
                {
                    snprintf(line, sizeof(line), "Liste limitée aux %d premiers jeux.", SDSCAN_MAX);
                    gfx_text_box(c, &font_small, x, y, w, ALIGN_LEFT, line, COL_WARN);
                    y += font_small.height + 1;
                }
                gfx_text_wrap(c, &font_small, x, y, w, r.y + r.h - 4 - y,
                              "Touche un jeu pour voir sa fiche : sauvegardes, suppression…",
                              COL_TEXT_OFF);
                break;
            }

            if (st == INST_IDLE || st == INST_DONE)
            {
                char help[320];
                snprintf(help, sizeof(help),
                         "Colle le lien d'un projet GitHub : la dernière release "
                         "est téléchargée automatiquement.\n"
                         "\n"
                         "\xE2\x80\xA2 github.com/auteur/projet\n"
                         "\xE2\x80\xA2 auteur/projet\n"
                         "\xE2\x80\xA2 https://site.com/jeu.nds (.dsi, .zip)\n"
                         "\n"
                         "Le jeu est copié dans %s, puis la console redémarre.",
                         installer_dir());
                gfx_text_wrap(c, &font_small, r.x + 8, r.y + 6, r.w - 16, r.h - 12,
                              help, COL_TEXT_DIM);
                break;
            }

            if (st == INST_ERROR)
            {
                gfx_text(c, &font_body, r.x + 8, r.y + 6, "Échec de l'installation",
                         COL_ERR);
                gfx_text_wrap(c, &font_small, r.x + 8, r.y + 8 + font_body.height,
                              r.w - 16, r.h - 16 - font_body.height,
                              installer_message(), COL_TEXT);
                break;
            }

            if (st == INST_PREVIEW)
            {
                // Pré-description : ce qui va être installé
                const install_info_t *i = installer_preview_info();
                int x = r.x + 8, w = r.w - 16, y = r.y + 6;
                char size[24], free_s[24], line[160];

                gfx_text_box(c, &font_body, x, y, w, ALIGN_LEFT, i->name, COL_TEXT);
                y += font_body.height + 3;
                if (i->project[0])
                    y += draw_field(c, x, y, w, "Projet", i->project, COL_TEXT);
                if (i->author[0])
                    y += draw_field(c, x, y, w, "Auteur", i->author, COL_TEXT);
                else if (!i->github && i->source[0])
                    y += draw_field(c, x, y, w, "Site", i->source, COL_TEXT);
                if (i->version[0])
                    y += draw_field(c, x, y, w, "Version", i->version, COL_TEXT);

                uint64_t free_b = installer_free_space();
                format_size(free_s, sizeof(free_s), free_b);
                uint16_t size_col = COL_TEXT;
                if (i->size == 0)
                    snprintf(line, sizeof(line), "inconnue");
                else
                {
                    format_size(size, sizeof(size), i->size);
                    if (free_b && i->size > free_b)
                    {
                        snprintf(line, sizeof(line), "%s : pas assez de place (%s libre)", size,
                                 free_s);
                        size_col = COL_ERR;
                    }
                    else if (installer_verbose() && free_b)
                        snprintf(line, sizeof(line), "%s (%s libre sur la SD)", size, free_s);
                    else
                        snprintf(line, sizeof(line), "%s", size);
                }
                y += draw_field(c, x, y, w, "Taille", line, size_col);
                if (installer_verbose() && i->archive)
                    y += draw_field(c, x, y, w, "Format", "archive .zip (la ROM sera extraite)",
                                    COL_TEXT_DIM);
                y += draw_field(c, x, y, w, "Dossier", installer_dir(), COL_TEXT_DIM);
                break;
            }

            // Liste des étapes
            static const struct {
                inst_state_t state;
                const char *label;
            } steps[] = {
                { INST_RESOLVING, "Recherche de la release" },
                { INST_DOWNLOADING, "Téléchargement" },
                { INST_EXTRACTING, "Extraction de l'archive" },
                { INST_VERIFYING, "Vérification et copie sur la SD" },
            };

            int y = r.y + 6;
            for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
            {
                int cy = y + font_body.height / 2;
                uint16_t col = COL_TEXT_OFF;
                if (steps[i].state < st)
                {
                    gfx_fill_circle(c, r.x + 14, cy, 6, COL_OK);
                    draw_check(c, r.x + 13, cy, COL_BG2);
                    col = COL_TEXT_DIM;
                }
                else if (steps[i].state == st)
                {
                    ui_spinner(c, r.x + 14, cy, 5, app.frame, COL_ACCENT);
                    col = COL_TEXT;
                }
                else
                {
                    gfx_fill_circle(c, r.x + 14, cy, 3, COL_BORDER);
                }
                gfx_text(c, &font_body, r.x + 28, y, steps[i].label, col);
                y += font_body.height + 1;
            }

            y += 3;
            gfx_hline(c, r.x + 8, y, r.w - 16, COL_BORDER);
            y += 4;

            char line[192];
            const uint64_t GB = 1024ull * 1024 * 1024;
            uint64_t total = installer_progress_total();
            bool big = st == INST_DOWNLOADING && total > GB;

            if (installer_verbose() && installer_log_count() > 0)
            {
                // Mode détaillé : le journal des dernières étapes, avec leur durée
                int room = (r.y + r.h - 4 - y) / font_small.height - (big ? 1 : 0);
                int n = installer_log_count();
                for (int k = n > room ? n - room : 0; k < n; k++)
                {
                    gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT,
                                 installer_log_get(k), k == n - 1 ? COL_TEXT : COL_TEXT_DIM);
                    y += font_small.height;
                }
            }
            else
            {
                if (installer_file_name()[0])
                {
                    snprintf(line, sizeof(line), "Fichier : %s", installer_file_name());
                    gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT, line,
                                 COL_TEXT_DIM);
                    y += font_small.height + 2;
                }
                if (installer_release()[0])
                {
                    snprintf(line, sizeof(line), "Release : %s", installer_release());
                    gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT, line,
                                 COL_TEXT_DIM);
                    y += font_small.height + 2;
                }
            }

            // Gros fichiers : stockés en morceaux de 1 Go (limite FAT32 : 4 Go)
            if (big)
            {
                snprintf(line, sizeof(line), "Découpé en %u morceaux de 1 Go sur la SD",
                         (unsigned)((total + GB - 1) / GB));
                gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT, line,
                             COL_ACCENT);
            }
            break;
        }

        case SCREEN_INSTALL_DONE:
        {
            gfx_text(c, &font_title, 12, 30,
                     installer_last_was_update() ? "C'est à jour !" : "C'est installé !",
                     COL_OK);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);

            char line[288];
            int y = r.y + 8;
            gfx_text_box(c, &font_body, r.x + 8, y, r.w - 16, ALIGN_LEFT,
                         installer_file_name(), COL_TEXT);
            y += font_body.height + 4;

            if (installer_release()[0])
            {
                snprintf(line, sizeof(line), "Version : %s", installer_release());
                gfx_text_box(c, &font_small, r.x + 8, y, r.w - 16, ALIGN_LEFT, line,
                             COL_TEXT_DIM);
                y += font_small.height + 2;
            }

            snprintf(line, sizeof(line), "Emplacement : %s", installer_dest_path());
            y += gfx_text_wrap(c, &font_small, r.x + 8, y, r.w - 16,
                               font_small.height * 2, line, COL_TEXT_DIM);
            y += 8;

            if (installer_last_was_update())
                snprintf(line, sizeof(line), "Tes sauvegardes sont conservées. "
                         "L'ancienne version est gardée de côté au cas où. "
                         "Redémarrage dans %d s.", reboot_seconds_left());
            else
                snprintf(line, sizeof(line), "La console redémarre dans %d s. "
                         "Le jeu apparaîtra dans ton menu (TWiLight Menu++…).",
                         reboot_seconds_left());
            gfx_text_wrap(c, &font_small, r.x + 8, y, r.w - 16, r.y + r.h - y - 4,
                          line, COL_TEXT);
            break;
        }

        case SCREEN_TWL:
        {
            gfx_text(c, &font_title, 12, 30, "TWiLight Menu++", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);

            twl_state_t st = installer_twl_state();
            const char *inst_ver = installer_twl_installed();
            char text[400];
            if (st == TWL_UPDATING)
                snprintf(text, sizeof(text),
                         "Mise à jour vers %s en cours.\n"
                         "\n"
                         "N'éteins pas la console et ne retire pas la carte SD.\n"
                         "\n"
                         "Si ça coupe quand même, relance la mise à jour : elle "
                         "reprend au paquet où elle s'était arrêtée.",
                         installer_twl_latest());
            else if (st == TWL_DONE)
                snprintf(text, sizeof(text),
                         "TWiLight Menu++ %s est installé.\n"
                         "\n"
                         "Redémarre la console pour l'utiliser. Tes réglages, tes "
                         "jeux et tes sauvegardes n'ont pas été touchés.",
                         installer_twl_latest());
            else
                snprintf(text, sizeof(text),
                         "Version installée : %s\n"
                         "Dernière version : %s\n"
                         "\n"
                         "La mise à jour est téléchargée en petits paquets, vérifiés "
                         "puis installés un par un sur la carte SD. Tes réglages, tes "
                         "jeux et tes sauvegardes sont conservés.",
                         inst_ver[0] ? inst_ver : "inconnue",
                         installer_twl_latest()[0] ? installer_twl_latest() : "…");
            gfx_text_wrap(c, &font_small, r.x + 8, r.y + 6, r.w - 16, r.h - 12, text,
                          COL_TEXT_DIM);
            break;
        }

        case SCREEN_ABOUT:
        {
            gfx_text(c, &font_title, 12, 30, "À propos", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);
            gfx_text_wrap(c, &font_small, r.x + 8, r.y + 6, r.w - 16, r.h - 12,
                          APP_NAME " v" APP_VERSION "\n"
                          "Application homebrew pour Nintendo DSi.\n"
                          "\n"
                          "Compilée avec BlocksDS (libnds + DSWiFi).\n"
                          "Police : Inter (SIL Open Font License).\n"
                          "\n"
                          "Base prête à l'emploi : rendu 2D logiciel sur les "
                          "deux écrans, interface tactile, clavier virtuel, "
                          "Wi-Fi WPA2 et client HTTP.",
                          COL_TEXT_DIM);
            break;
        }
    }
}

// Résumé de ce qui est affiché en haut : si ça ne change pas, on ne redessine pas
static uint32_t top_signature(void)
{
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(app.screen);
    MIX(net_state());
    MIX((uintptr_t)net_state_text());
    MIX(http_state());
    MIX(http_bytes_received());
    MIX((uintptr_t)http_progress_text()[0]);
    MIX(net_signal_bars());
    MIX(installer_state());
    MIX(app.install_tab);
    MIX(app.sdscan.count);
    MIX(app.sdscan.dirs);
    MIX(app.sdscan.done);
    MIX(app.sd_started);
    MIX(installer_log_count());
    MIX(installer_verbose());
    MIX(installer_progress_now() / 8192);
    MIX(installer_progress_total());
    MIX(strlen(installer_message()));
    MIX((unsigned char)installer_message()[0]);
    if (app.screen == SCREEN_INSTALL_DONE)
        MIX(reboot_seconds_left());
    if (app.screen == SCREEN_LAUNCH)
        MIX((LAUNCH_DELAY_FRAMES - (int)(app.frame - app.launch_frame)) / 60);
    MIX(time(NULL) / 60);
    MIX(lremote_enabled());
    MIX(lremote_net_running());
    MIX(lremote_sessions());
    MIX(installer_checking());
    if (installer_checking())
    {
        int done, total;
        installer_check_progress(&done, &total);
        MIX(done);
    }
    MIX(installer_self_state());
    MIX(installer_twl_state());
    MIX(strlen(installer_twl_installed()));
    MIX(strlen(installer_twl_latest()));
    MIX(remote_state());
    MIX(strlen(remote_status_text()));
    MIX(strlen(remote_owner_name()));
    for (const char *p = remote_pair_code(); *p; p++)
        MIX((unsigned char)*p);
    if (remote_state() == REMOTE_CONNECTING)
        MIX(app.frame / 5);
    if (net_busy())
        MIX(app.frame / 5); // Animation du spinner
#undef MIX
    return h;
}

// ---------------------------------------------------------------------------
// Écran du bas
// ---------------------------------------------------------------------------

#define BTN_X0 16
#define BTN_W  (SCREEN_W - 32)
#define BTN_H  30

static void screen_home(canvas_t *c, const input_t *in)
{
    (void)in;
    ui_header(c, APP_NAME, APP_VERSION);

    if (!app.games_loaded)
        refresh_games();

    // Les deux grandes tuiles : installer, et gérer tous les jeux de la carte SD
    if (ui_tile(c, RECT(8, 27, 116, 84), "Installer un jeu", "GitHub ou un lien",
                ICON_DOWNLOAD, true))
    {
        app.install_tab = TAB_INSTALL;
        go(SCREEN_INSTALL);
    }
    if (ui_tile(c, RECT(132, 27, 116, 84), "Gérer les jeux", "tous ceux de la SD",
                ICON_GAMES, false))
    {
        app.install_tab = TAB_SD;
        sd_enter();
        go(SCREEN_INSTALL);
    }

    // Raccourcis
    const int bw = (SCREEN_W - 16 - 6) / 2;
    if (ui_button(c, RECT(8, 115, bw, 22),
                  net_state() == NET_CONNECTED ? "Wi-Fi \xE2\x80\xA2" : "Wi-Fi",
                  BTN_STYLE_NORMAL, app.wifi_ok))
        go(SCREEN_WIFI);
    if (ui_button(c, RECT(8 + bw + 6, 115, bw, 22), "Plus…", BTN_STYLE_NORMAL, true))
        go(SCREEN_MORE);

    const char *warn = NULL;
    if (!app.wifi_ok)
        warn = "Le Wi-Fi n'a pas pu être initialisé";
    else if (!installer_storage_ok())
        warn = "Carte SD introuvable : installation impossible";
    if (warn)
    {
        gfx_text_box(c, &font_small, 0, 139, SCREEN_W, ALIGN_CENTER, warn, COL_ERR);
    }
    else
    {
        char line[96];
        snprintf(line, sizeof(line), "Wi-Fi : %s", net_state_text());
        gfx_text_box(c, &font_small, 0, 139, SCREEN_W, ALIGN_CENTER, line, COL_TEXT_OFF);
    }

    // Remote : une case en bas de l'écran
    const char *rsub = remote_state() == REMOTE_PAIRED ? "Connecté"
                     : lremote_enabled() ? "Local actif"
                                         : "Commander la DSi depuis un téléphone";
    bool active = remote_state() == REMOTE_PAIRED ||
                  (lremote_enabled() && lremote_net_running());
    if (ui_remote_box(c, RECT(8, 155, SCREEN_W - 16, 34), "Remote", rsub, active))
    {
        // Onglet par défaut : en ligne s'il y a un serveur et que la DSi y est
        // déjà associée, sinon le Remote local
        if (remote_state() == REMOTE_NO_SERVER)
            app.remote_tab = RTAB_LOCAL;
        else if (remote_has_device() && !lremote_enabled())
            app.remote_tab = RTAB_ONLINE;
        if (app.remote_tab == RTAB_ONLINE)
        {
            remote_start();
            remote_request_pair_code();
        }
        app.lr_editing = false;
        go(SCREEN_REMOTE);
    }
}

static void screen_more(canvas_t *c, const input_t *in)
{
    ui_header(c, "Plus", NULL);

    const int bh = 24, step = 27;
    int y = 28;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Mettre à jour TWiLight Menu++",
                  BTN_STYLE_NORMAL, true))
    {
        go(SCREEN_TWL);
        // Demande tout de suite la dernière version (sauf mise à jour en cours)
        if (installer_twl_present() && net_state() == NET_CONNECTED && !twl_busy())
            installer_twl_check();
    }
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Réglages", BTN_STYLE_NORMAL, true))
        go(SCREEN_SETTINGS);
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Test Internet (HTTP)",
                  BTN_STYLE_NORMAL, net_state() == NET_CONNECTED))
        go(SCREEN_HTTP);
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "À propos", BTN_STYLE_NORMAL, true))
        go(SCREEN_ABOUT);
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Quitter DSi Shop",
                  BTN_STYLE_DANGER, true))
        app.quit = true;
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_HOME);
}

// Réglages : thème, Verbose, lancement au démarrage
static void screen_settings(canvas_t *c, const input_t *in)
{
    ui_header(c, "Réglages", NULL);

    const int bh = 26, step = 32;
    int y = 30;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh),
                  ui_theme_is_light() ? "Thème : clair" : "Thème : sombre",
                  BTN_STYLE_NORMAL, true))
    {
        bool light = !ui_theme_is_light();
        ui_set_theme(light);
        installer_pref_set("theme", light ? "light" : "dark");
        app.force_redraw = true;   // Redessine aussi l'écran du haut
    }
    y += step;

    // Verbose : étapes détaillées pendant une installation (oui par défaut)
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh),
                  installer_verbose() ? "Verbose : OUI" : "Verbose : NON",
                  BTN_STYLE_NORMAL, true))
    {
        installer_set_verbose(!installer_verbose());
        ui_toast(installer_verbose() ? "Étapes détaillées pendant l'installation"
                                     : "Étapes résumées pendant l'installation");
    }
    y += step;

    // TWiLight Menu++ relance tout seul la dernière ROM (ici : DSi Shop)
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh),
                  app.autoboot ? "Au démarrage : DSi Shop" : "Au démarrage : TWiLight Menu++",
                  BTN_STYLE_NORMAL, installer_autoboot_available()))
    {
        if (!installer_autoboot_set(!app.autoboot))
            ui_toast("Réglages de TWiLight non modifiables");
        else
        {
            app.autoboot = !app.autoboot;
            ui_toast(app.autoboot ? "Maintiens B au démarrage : TWiLight"
                                  : "Au démarrage : menu TWiLight");
        }
    }
    y += step;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, bh), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_MORE);

    gfx_text_wrap(c, &font_small, 12, SCREEN_H - 30, SCREEN_W - 24, 28,
                  "Verbose : détaille les étapes d'une installation (journal, débit).",
                  COL_TEXT_OFF);
}

// Remote en ligne (serveur + compte Google)
static bool remote_online(canvas_t *c)
{
    remote_state_t rs = remote_state();
    const int col_x = 128, col_w = SCREEN_W - col_x - 8;

    if (rs == REMOTE_NO_SERVER)
    {
        gfx_text_wrap(c, &font_small, 16, 56, SCREEN_W - 32, 90,
                      "Aucun serveur Remote n'est configuré (voir l'écran du haut). "
                      "Le Remote local, lui, marche sans serveur.", COL_TEXT_DIM);
        return ui_button(c, RECT(BTN_X0, 150, BTN_W, 26), "Retour", BTN_STYLE_NORMAL, true);
    }

    rect_t qr = RECT(6, 50, 116, 116);
    const char *url = remote_pair_url();

    if (url[0])
    {
        draw_qr(c, qr, url);
        gfx_text_box(c, &font_small, col_x, 52, col_w, ALIGN_LEFT,
                     rs == REMOTE_PAIRED ? "Associer un autre :" : "Ou tape le code :",
                     COL_TEXT_DIM);
        gfx_text_box(c, &font_body, col_x, 52 + font_small.height, col_w, ALIGN_LEFT,
                     remote_pair_code(), COL_TEXT);
    }
    else
    {
        gfx_fill_round_rect(c, qr.x + 6, qr.y + 6, qr.w - 12, qr.h - 12, 8, COL_PANEL);
        if (rs == REMOTE_WAIT_WIFI)
            gfx_text_wrap(c, &font_small, qr.x + 14, qr.y + 40, qr.w - 28, 60,
                          "Connecte-toi d'abord au Wi-Fi", COL_TEXT_DIM);
        else if (rs == REMOTE_ERROR)
            gfx_text_wrap(c, &font_small, qr.x + 14, qr.y + 30, qr.w - 28, 80,
                          remote_status_text(), COL_ERR);
        else
            ui_spinner(c, qr.x + qr.w / 2, qr.y + qr.h / 2, 8, app.frame, COL_ACCENT);
    }

    char line[96];
    if (rs == REMOTE_PAIRED)
        snprintf(line, sizeof(line), "Connecté à %s", remote_owner_name());
    else
        snprintf(line, sizeof(line), "%s", remote_status_text());
    gfx_text_wrap(c, &font_small, col_x, 86, col_w, font_small.height * 3, line,
                  rs == REMOTE_PAIRED ? COL_OK : COL_TEXT_DIM);

    if (rs == REMOTE_WAIT_WIFI)
    {
        if (ui_button(c, RECT(col_x, 126, col_w, 22), "Wi-Fi", BTN_STYLE_PRIMARY,
                      app.wifi_ok))
            go(SCREEN_WIFI);
    }
    else if (rs == REMOTE_PAIRED)
    {
        if (ui_button(c, RECT(col_x, 126, col_w, 22), "Délier", BTN_STYLE_DANGER, true))
        {
            remote_forget();
            ui_toast("DSi déliée : un nouveau QR code arrive");
        }
    }
    else
    {
        if (ui_button(c, RECT(col_x, 126, col_w, 22), "Nouveau code", BTN_STYLE_NORMAL,
                      rs == REMOTE_WAIT_PAIR))
            remote_request_pair_code();
    }

    return ui_button(c, RECT(col_x, 152, col_w, 22), "Retour", BTN_STYLE_NORMAL, true);
}

// Mot de passe masqué (on voit seulement le dernier caractère tapé)
static void masked(const char *pass, bool show_last, char *out, size_t size)
{
    size_t n = strlen(pass), o = 0;
    for (size_t i = 0; i < n && o + 4 < size; i++)
    {
        if (show_last && i == n - 1)
            out[o++] = pass[i];
        else
        {
            memcpy(out + o, "\xE2\x80\xA2", 3); // •
            o += 3;
        }
    }
    out[o] = '\0';
}

// Remote local (la DSi sert la page elle-même)
static bool remote_local(canvas_t *c)
{
    // 1. Pas encore d'identifiants (ou on les change) : formulaire
    if (!lremote_configured() || app.lr_editing)
    {
        gfx_text(c, &font_small, 10, 50, "Identifiant", COL_TEXT_DIM);
        rect_t fu = RECT(8, 62, SCREEN_W - 16, 24);
        bool pressed, focused;
        if (ui_hit(fu, true, &pressed, &focused))
        {
            app.lr_field = 0;
            kbd_open(app.lr_user, sizeof(app.lr_user));
            go(SCREEN_LR_EDIT);
            return false;
        }
        draw_text_field(c, fu, app.lr_user, "ex. moi", false, focused || pressed);

        char label[48];
        snprintf(label, sizeof(label), "Mot de passe (%d caractères min.)", LREMOTE_PASS_MIN);
        gfx_text(c, &font_small, 10, 90, label, COL_TEXT_DIM);
        rect_t fp = RECT(8, 102, SCREEN_W - 16, 24);
        if (ui_hit(fp, true, &pressed, &focused))
        {
            app.lr_field = 1;
            kbd_open(app.lr_pass, sizeof(app.lr_pass));
            go(SCREEN_LR_EDIT);
            return false;
        }
        char hidden[LREMOTE_PASS_MAX * 3 + 1];
        masked(app.lr_pass, false, hidden, sizeof(hidden));
        draw_text_field(c, fp, hidden, "••••••", false, focused || pressed);

        if (ui_button(c, RECT(8, 134, SCREEN_W - 16, 24), "Enregistrer et activer",
                      BTN_STYLE_PRIMARY, app.lr_user[0] && app.lr_pass[0]))
        {
            char err[96];
            if (lremote_set_credentials(app.lr_user, app.lr_pass, err, sizeof(err)))
            {
                memset(app.lr_pass, 0, sizeof(app.lr_pass));
                app.lr_editing = false;
                app.force_redraw = true;
                ui_toast("Remote local activé");
            }
            else
            {
                ui_toast(err);
            }
        }
        return ui_button(c, RECT(8, 164, SCREEN_W - 16, 22),
                         app.lr_editing ? "Annuler" : "Retour", BTN_STYLE_NORMAL, true);
    }

    // 2. Configuré
    const int col_x = 128, col_w = SCREEN_W - col_x - 8;
    rect_t qr = RECT(6, 50, 116, 116);
    const char *url = lremote_glue_url();
    bool on = lremote_enabled();

    if (on && url[0] && !lremote_glue_failed())
    {
        draw_qr(c, qr, url);
    }
    else
    {
        gfx_fill_round_rect(c, qr.x + 6, qr.y + 6, qr.w - 12, qr.h - 12, 8, COL_PANEL);
        const char *why = !on ? "Remote local désactivé"
                        : !url[0] ? "Connecte-toi d'abord au Wi-Fi"
                                  : "Le serveur n'a pas pu démarrer";
        gfx_text_wrap(c, &font_small, qr.x + 14, qr.y + 40, qr.w - 28, 60, why,
                      lremote_glue_failed() ? COL_ERR : COL_TEXT_DIM);
    }

    int y = 50;
    gfx_text(c, &font_small, col_x, y, "Adresse", COL_TEXT_DIM);
    y += font_small.height;
    char addr[40] = "—";
    if (url[0])
    {
        snprintf(addr, sizeof(addr), "%s", url + 7); // Sans "http://"
        addr[strcspn(addr, "/")] = '\0';            // ni "/" final
    }
    gfx_text_box(c, &font_body, col_x, y, col_w, ALIGN_LEFT, addr, COL_TEXT);
    y += font_body.height + 2;
    gfx_text(c, &font_small, col_x, y, "Identifiant", COL_TEXT_DIM);
    y += font_small.height;
    gfx_text_box(c, &font_body, col_x, y, col_w, ALIGN_LEFT, lremote_user(), COL_TEXT);
    y += font_body.height + 2;

    int n = lremote_sessions();
    char line[48];
    if (n)
        snprintf(line, sizeof(line), "%d téléphone(s) connecté(s)", n);
    else
        snprintf(line, sizeof(line), on ? "En attente…" : "");
    gfx_text_box(c, &font_small, col_x, y, col_w, ALIGN_LEFT, line,
                 n ? COL_OK : COL_TEXT_OFF);

    if (!url[0] && on)
    {
        if (ui_button(c, RECT(col_x, 130, col_w, 20), "Wi-Fi", BTN_STYLE_PRIMARY,
                      app.wifi_ok))
            go(SCREEN_WIFI);
    }
    else if (ui_button(c, RECT(col_x, 130, col_w, 20), on ? "Désactiver" : "Activer",
                       on ? BTN_STYLE_DANGER : BTN_STYLE_PRIMARY, true))
    {
        lremote_set_enabled(!on);
        ui_toast(on ? "Remote local désactivé" : "Remote local activé");
    }

    if (ui_button(c, RECT(col_x, 152, col_w / 2 - 2, 20), "Identif.", BTN_STYLE_NORMAL,
                  true))
    {
        snprintf(app.lr_user, sizeof(app.lr_user), "%s", lremote_user());
        app.lr_pass[0] = '\0';
        app.lr_editing = true;
        app.force_redraw = true;
    }
    return ui_button(c, RECT(col_x + col_w / 2 + 2, 152, col_w / 2 - 2, 20), "Retour",
                     BTN_STYLE_NORMAL, true);
}

static void screen_remote(canvas_t *c, const input_t *in)
{
    ui_header(c, "Remote", NULL);

    const char *const tabs[2] = { "Local (Wi-Fi maison)", "En ligne (Google)" };
    int tab = ui_tabs(c, RECT(8, 26, SCREEN_W - 16, 20), tabs, 2, app.remote_tab);
    if (tab != app.remote_tab)
    {
        app.remote_tab = tab;
        if (tab == RTAB_ONLINE)
        {
            remote_start();
            remote_request_pair_code();
        }
        app.force_redraw = true;
    }

    bool back = app.remote_tab == RTAB_LOCAL ? remote_local(c) : remote_online(c);

    if (back || (in->down & BTN_B))
    {
        if (app.lr_editing)
        {
            app.lr_editing = false;
            memset(app.lr_pass, 0, sizeof(app.lr_pass));
            app.force_redraw = true;
        }
        else
        {
            go(SCREEN_HOME);
        }
    }
}

static void screen_lr_edit(canvas_t *c, const input_t *in)
{
    bool pass = app.lr_field == 1;
    ui_header(c, pass ? "Mot de passe du Remote local" : "Identifiant du Remote local",
              NULL);

    gfx_text_box(c, &font_small, 10, 28, SCREEN_W - 20, ALIGN_LEFT,
                 pass ? "6 caractères minimum. Il te sera demandé sur le téléphone."
                      : "3 caractères minimum, sans espace.", COL_TEXT_DIM);

    rect_t f = RECT(8, 42, SCREEN_W - 16, 26);
    if (pass)
    {
        char hidden[LREMOTE_PASS_MAX * 3 + 1];
        masked(app.lr_pass, true, hidden, sizeof(hidden));
        draw_text_field(c, f, hidden, NULL, true, true);
    }
    else
    {
        draw_text_field(c, f, app.lr_user, NULL, true, true);
    }

    if (kbd_update(c, in, 88) != KBD_EDITING)
    {
        app.lr_editing = lremote_configured(); // Garde le formulaire ouvert
        go(SCREEN_REMOTE);
    }
}

static void screen_wifi(canvas_t *c, const input_t *in)
{
    ui_header(c, "Wi-Fi", NULL);

    net_state_t s = net_state();
    bool connected = (s == NET_CONNECTED || s == NET_CONNECTING);

    int y = 32;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Réseaux enregistrés (DSi)",
                  BTN_STYLE_PRIMARY, !connected))
    {
        net_connect_saved();
        ui_toast("Connexion aux réseaux enregistrés…");
    }
    y += BTN_H + 6;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Rechercher des réseaux",
                  BTN_STYLE_NORMAL, true))
    {
        net_scan_start();
        app.scan_scroll = 0;
        go(SCREEN_SCAN);
    }
    y += BTN_H + 6;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Se déconnecter",
                  BTN_STYLE_DANGER, connected))
    {
        net_disconnect();
        ui_toast("Déconnecté");
    }
    y += BTN_H + 6;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_HOME);
}

#define LIST_Y     28
#define ITEM_H     30
#define ITEM_GAP   4
#define LIST_ROWS  4

static void pick_network(int index)
{
    if (!net_scan_get(index, &app.selected))
        return;
    app.selected_ap = index;

    if (!app.selected.compatible)
    {
        ui_toast(net_is_dsi_mode() ? "Réseau incompatible"
                                   : "WPA/WPA2 indisponible en mode DS");
        return;
    }

    if (app.selected.security == NET_SEC_OPEN || app.selected.saved)
    {
        if (net_connect_ap(index, NULL))
            go(SCREEN_WIFI);
        else
            ui_toast("Impossible de lancer la connexion");
        return;
    }

    app.password[0] = '\0';
    kbd_open(app.password, sizeof(app.password));
    go(SCREEN_PASSWORD);
}

static void screen_scan(canvas_t *c, const input_t *in)
{
    int count = net_scan_count();

    char title[32];
    snprintf(title, sizeof(title), "Réseaux trouvés : %d", count);
    ui_header(c, title, NULL);

    if (count == 0)
    {
        ui_spinner(c, SCREEN_W / 2, 80, 8, app.frame, COL_ACCENT);
        gfx_text_box(c, &font_small, 0, 98, SCREEN_W, ALIGN_CENTER,
                     "Recherche en cours…", COL_TEXT_DIM);
    }

    int max_scroll = count - LIST_ROWS;
    if (max_scroll < 0)
        max_scroll = 0;
    if (in->repeat & BTN_L)
        app.scan_scroll--;
    if (in->repeat & BTN_R)
        app.scan_scroll++;
    if (app.scan_scroll > max_scroll)
        app.scan_scroll = max_scroll;
    if (app.scan_scroll < 0)
        app.scan_scroll = 0;

    for (int row = 0; row < LIST_ROWS; row++)
    {
        int i = app.scan_scroll + row;
        net_ap_t ap;
        if (!net_scan_get(i, &ap))
            break;

        char sub[48];
        snprintf(sub, sizeof(sub), "%s · canal %d%s", security_name(ap.security),
                 ap.channel, ap.saved ? " · enregistré" : "");

        rect_t r = RECT(8, LIST_Y + row * (ITEM_H + ITEM_GAP), SCREEN_W - 16 - 26, ITEM_H);
        // Réserve la place de l'icône de signal à droite (texte vide)
        if (ui_list_item(c, r, ap.ssid, sub, "      ", COL_TEXT_DIM))
            pick_network(i);
        ui_wifi_icon(c, r.x + r.w - 22, r.y + (ITEM_H - 12) / 2, ap_bars(&ap),
                     ap.compatible ? COL_ACCENT : COL_TEXT_OFF, COL_BORDER);
    }

    // Barre de défilement
    if (count > LIST_ROWS)
    {
        int track_y = LIST_Y, track_h = LIST_ROWS * (ITEM_H + ITEM_GAP) - ITEM_GAP;
        int thumb_h = track_h * LIST_ROWS / count;
        int thumb_y = track_y + (track_h - thumb_h) * app.scan_scroll / max_scroll;
        gfx_fill_round_rect(c, SCREEN_W - 12, track_y, 4, track_h, 2, COL_PANEL);
        gfx_fill_round_rect(c, SCREEN_W - 12, thumb_y, 4, thumb_h, 2, COL_ACCENT);
    }

    // Boutons du bas
    int by = SCREEN_H - 28;
    int bw = (SCREEN_W - 16 - 8 * 2) / 3;
    if (ui_button(c, RECT(8, by, bw, 22), "\xE2\x86\x91 L", BTN_STYLE_NORMAL,
                  app.scan_scroll > 0))
        app.scan_scroll--;
    if (ui_button(c, RECT(8 + bw + 8, by, bw, 22), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
    {
        net_scan_stop();
        go(SCREEN_WIFI);
    }
    if (ui_button(c, RECT(8 + (bw + 8) * 2, by, bw, 22), "R \xE2\x86\x93",
                  BTN_STYLE_NORMAL, app.scan_scroll < max_scroll))
        app.scan_scroll++;
}

static void screen_password(canvas_t *c, const input_t *in)
{
    ui_header(c, "Mot de passe", security_name(app.selected.security));

    gfx_text_box(c, &font_small, 10, 28, SCREEN_W - 20, ALIGN_LEFT,
                 app.selected.ssid, COL_TEXT_DIM);

    rect_t f = RECT(8, 42, SCREEN_W - 16, 26);
    draw_text_field(c, f, app.password, NULL, true, true);

    char count[16];
    snprintf(count, sizeof(count), "%u car.", (unsigned)strlen(app.password));
    gfx_text(c, &font_small, f.x + f.w - 8 - gfx_text_width(&font_small, count),
             f.y + f.h + 1, count, COL_TEXT_OFF);

    kbd_result_t r = kbd_update(c, in, 88);

    if (r == KBD_CANCELLED)
    {
        go(SCREEN_SCAN);
    }
    else if (r == KBD_ACCEPTED)
    {
        size_t len = strlen(app.password);
        bool valid = true;
        if (app.selected.security == NET_SEC_WEP)
            valid = (len == 5 || len == 13 || len == 16);
        else
            valid = (len >= 8 && len <= 64);

        if (!valid)
        {
            ui_toast(app.selected.security == NET_SEC_WEP
                         ? "Clé WEP : 5, 13 ou 16 caractères"
                         : "Le mot de passe doit faire 8 à 64 caractères");
        }
        else if (net_connect_ap(app.selected_ap, app.password))
        {
            go(SCREEN_WIFI);
        }
        else
        {
            ui_toast("Impossible de lancer la connexion");
        }
    }
}

static void screen_http(canvas_t *c, const input_t *in)
{
    ui_header(c, "Test Internet", NULL);

    gfx_text_wrap(c, &font_small, 16, 32, SCREEN_W - 32, 40,
                  "Télécharge http://" TEST_HOST TEST_PATH " et affiche la "
                  "réponse sur l'écran du haut.", COL_TEXT_DIM);

    bool busy = http_state() == HTTP_BUSY;
    bool online = net_state() == NET_CONNECTED;

    if (ui_button(c, RECT(BTN_X0, 80, BTN_W, BTN_H),
                  busy ? "Requête en cours…" : "Lancer la requête",
                  BTN_STYLE_PRIMARY, !busy && online))
    {
        if (!http_get_async(TEST_HOST, TEST_PATH))
            ui_toast("Impossible de lancer la requête");
    }

    if (ui_button(c, RECT(BTN_X0, 80 + BTN_H + 8, BTN_W, BTN_H), "Retour",
                  BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_MORE);

    if (!online)
        gfx_text_box(c, &font_small, 0, SCREEN_H - 22, SCREEN_W, ALIGN_CENTER,
                     "Connecte-toi d'abord au Wi-Fi", COL_WARN);
}

// Ouvre la fiche d'un jeu de DSi Shop (dossier d'installation)
static void open_game(const game_t *g)
{
    app.sel = *g;
    app.sel_from_sd = false;
    snprintf(app.sel_dir, sizeof(app.sel_dir), "%s", installer_dir());
    app.sel_saves = games_find_saves(app.sel_dir, g->file, NULL, 0);
    app.sel_backup = games_has_backup(app.sel_dir, g->file);
    snprintf(app.sel_kind, sizeof(app.sel_kind), "%s", installer_rom_kind_text(app.sel_dir, g->file));
    go(SCREEN_GAME);
}

// Ouvre la fiche d'un jeu trouvé sur la carte (n'importe quel dossier)
static void open_sd_game(const sdgame_t *sg)
{
    char dir[SDSCAN_PATH];
    sdgame_folder(sg, dir, sizeof(dir));

    game_t g;
    games_get_record(dir, sdgame_name(sg), &g);   // Origine connue si DSi Shop l'a installé
    g.size = sg->size;

    app.sel = g;
    app.sel_from_sd = true;
    snprintf(app.sel_dir, sizeof(app.sel_dir), "%s", dir);
    app.sel_saves = games_find_saves(app.sel_dir, g.file, NULL, 0);
    app.sel_backup = games_has_backup(app.sel_dir, g.file);
    snprintf(app.sel_kind, sizeof(app.sel_kind), "%s", installer_rom_kind_text(app.sel_dir, g.file));
    go(SCREEN_GAME);
}

// Recharge la fiche du jeu affiché (après une mise à jour par exemple)
static void reload_selected(void)
{
    if (!app.sel_from_sd)
        for (int i = 0; i < app.ngames; i++)
            if (strcmp(app.games[i].file, app.sel.file) == 0)
                app.sel = app.games[i];
    app.sel_saves = games_find_saves(app.sel_dir, app.sel.file, NULL, 0);
    app.sel_backup = games_has_backup(app.sel_dir, app.sel.file);
}

static const char *update_label(const game_t *g, uint16_t *color)
{
    const char *latest;
    upd_status_t st = installer_update_status(g->file, &latest);
    *color = COL_TEXT_DIM;
    switch (st)
    {
        case UPD_CHECKING:   return "…";
        case UPD_AVAILABLE:  *color = COL_WARN; return "Màj !";
        case UPD_UP_TO_DATE: *color = COL_OK;   return "À jour";
        case UPD_ERROR:      *color = COL_ERR;  return "?";
        default:             (void)g; return "";
    }
}

#define GAMES_Y     52
#define GAME_ROW_H  32
#define GAME_ROWS   3

static void screen_games(canvas_t *c, const input_t *in)
{
    if (!app.games_loaded)
        refresh_games();

    int max_scroll = app.ngames - GAME_ROWS;
    if (max_scroll < 0)
        max_scroll = 0;
    if (in->repeat & BTN_L)
        app.games_scroll--;
    if (in->repeat & BTN_R)
        app.games_scroll++;
    if (app.games_scroll > max_scroll)
        app.games_scroll = max_scroll;
    if (app.games_scroll < 0)
        app.games_scroll = 0;

    if (app.ngames == 0)
    {
        char line[128];
        snprintf(line, sizeof(line), "Aucun jeu dans %s.\nInstalle ton premier jeu "
                 "avec l'onglet « Installer » ou depuis le Remote.", installer_dir());
        gfx_text_wrap(c, &font_small, 16, GAMES_Y + 16, SCREEN_W - 32, 80, line,
                      COL_TEXT_DIM);
    }

    for (int row = 0; row < GAME_ROWS; row++)
    {
        int i = app.games_scroll + row;
        if (i >= app.ngames)
            break;
        const game_t *g = &app.games[i];

        char size[24], sub[96];
        format_size(size, sizeof(size), g->size);
        if (g->version[0])
            snprintf(sub, sizeof(sub), "%s · %s", g->version, size);
        else
            snprintf(sub, sizeof(sub), "%s", size);

        rect_t r = RECT(8, GAMES_Y + row * (GAME_ROW_H + 4), SCREEN_W - 16 - 12,
                        GAME_ROW_H);
        uint16_t col;
        const char *badge = update_label(g, &col);
        if (ui_list_item(c, r, g->file, sub, badge, col))
        {
            open_game(g);
            return;
        }
    }

    if (app.ngames > GAME_ROWS)
    {
        int track_h = GAME_ROWS * (GAME_ROW_H + 4) - 4;
        int thumb_h = track_h * GAME_ROWS / app.ngames;
        int thumb_y = GAMES_Y + (track_h - thumb_h) * app.games_scroll / max_scroll;
        gfx_fill_round_rect(c, SCREEN_W - 12, GAMES_Y, 4, track_h, 2, COL_PANEL);
        gfx_fill_round_rect(c, SCREEN_W - 12, thumb_y, 4, thumb_h, 2, COL_ACCENT);
    }

    // Barre du bas : défilement, vérification des mises à jour, retour
    int by = SCREEN_H - 28;
    if (ui_button(c, RECT(8, by, 30, 22), "\xE2\x86\x91", BTN_STYLE_NORMAL,
                  app.games_scroll > 0))
        app.games_scroll--;

    char check[40] = "Vérifier les màj";
    bool checking = installer_checking();
    if (checking)
    {
        int done, total;
        installer_check_progress(&done, &total);
        snprintf(check, sizeof(check), "Vérification %d/%d", done, total);
    }
    bool online = net_state() == NET_CONNECTED;
    if (ui_button(c, RECT(42, by, 104, 22), check, BTN_STYLE_PRIMARY,
                  !checking && online && app.ngames > 0 && !install_busy()) ||
        ((in->down & BTN_Y) && online && !checking && !install_busy()))
    {
        if (!installer_check_updates(app.games, app.ngames))
            ui_toast("Impossible de vérifier maintenant");
    }

    if (ui_button(c, RECT(150, by, 64, 22), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_HOME);
    if (ui_button(c, RECT(218, by, 30, 22), "\xE2\x86\x93", BTN_STYLE_NORMAL,
                  app.games_scroll < max_scroll))
        app.games_scroll++;
}

// Onglet "Toute la SD" : tous les jeux (.nds / .dsi) de la carte, quel que soit leur dossier
static void screen_sd(canvas_t *c, const input_t *in)
{
    sdscan_t *s = &app.sdscan;
    bool scanning = app.sd_started && !s->done;

    int max_scroll = s->count - GAME_ROWS;
    if (max_scroll < 0)
        max_scroll = 0;
    if (in->repeat & BTN_L)
        app.sd_scroll--;
    if (in->repeat & BTN_R)
        app.sd_scroll++;
    if (app.sd_scroll > max_scroll)
        app.sd_scroll = max_scroll;
    if (app.sd_scroll < 0)
        app.sd_scroll = 0;

    if (!installer_storage_ok())
        gfx_text_wrap(c, &font_small, 16, GAMES_Y + 16, SCREEN_W - 32, 60,
                      "Carte SD introuvable.", COL_ERR);
    else if (s->count == 0)
        gfx_text_wrap(c, &font_small, 16, GAMES_Y + 16, SCREEN_W - 32, 80,
                      scanning ? "Analyse de la carte SD…"
                               : "Aucun jeu (.nds / .dsi) trouvé sur la carte SD.",
                      COL_TEXT_DIM);

    for (int row = 0; row < GAME_ROWS; row++)
    {
        int i = app.sd_scroll + row;
        if (i >= s->count)
            break;
        const sdgame_t *g = &s->games[i];

        char folder[SDSCAN_PATH], size[24], sub[SDSCAN_PATH + 32];
        sdgame_folder(g, folder, sizeof(folder));
        format_size(size, sizeof(size), g->size);
        snprintf(sub, sizeof(sub), "%s · %s", size, folder);

        bool in_shop = strcmp(folder, installer_dir()) == 0;
        rect_t r = RECT(8, GAMES_Y + row * (GAME_ROW_H + 4), SCREEN_W - 16 - 12, GAME_ROW_H);
        if (ui_list_item(c, r, sdgame_name(g), sub, in_shop ? "DSi Shop" : "",
                         COL_ACCENT))
        {
            open_sd_game(g);
            return;
        }
    }

    if (s->count > GAME_ROWS)
    {
        int track_h = GAME_ROWS * (GAME_ROW_H + 4) - 4;
        int thumb_h = track_h * GAME_ROWS / s->count;
        int thumb_y = GAMES_Y + (track_h - thumb_h) * app.sd_scroll / max_scroll;
        gfx_fill_round_rect(c, SCREEN_W - 12, GAMES_Y, 4, track_h, 2, COL_PANEL);
        gfx_fill_round_rect(c, SCREEN_W - 12, thumb_y, 4, thumb_h, 2, COL_ACCENT);
    }

    // Barre du bas : défilement, nouvelle analyse, retour
    int by = SCREEN_H - 28;
    if (ui_button(c, RECT(8, by, 30, 22), "\xE2\x86\x91", BTN_STYLE_NORMAL, app.sd_scroll > 0))
        app.sd_scroll--;

    char scan_label[40] = "Rescanner";
    if (scanning)
        snprintf(scan_label, sizeof(scan_label), "Analyse… %d", s->count);
    if (ui_button(c, RECT(42, by, 104, 22), scan_label, BTN_STYLE_PRIMARY,
                  !scanning && installer_storage_ok()))
        sd_start_scan();

    if (ui_button(c, RECT(150, by, 64, 22), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_HOME);
    if (ui_button(c, RECT(218, by, 30, 22), "\xE2\x86\x93", BTN_STYLE_NORMAL,
                  app.sd_scroll < max_scroll))
        app.sd_scroll++;
}

static void screen_install(canvas_t *c, const input_t *in)
{
    ui_header(c, app.install_tab == TAB_SD ? "Gérer les jeux" : "Installer un jeu", NULL);

    char games_label[32];
    snprintf(games_label, sizeof(games_label), "Mes jeux (%d)", app.ngames);
    const char *const tabs[3] = { "Installer", games_label, "Toute la SD" };
    int tab = ui_tabs(c, RECT(8, 26, SCREEN_W - 16, 20), tabs, 3, app.install_tab);
    if (tab != app.install_tab)
    {
        app.install_tab = tab;
        if (tab == TAB_GAMES)
            refresh_games();
        else if (tab == TAB_SD)
            sd_enter();
        app.force_redraw = true;
    }

    if (app.install_tab == TAB_GAMES)
    {
        screen_games(c, in);
        return;
    }
    if (app.install_tab == TAB_SD)
    {
        screen_sd(c, in);
        return;
    }

    bool busy = install_busy();
    bool online = net_state() == NET_CONNECTED;

    if (installer_state() == INST_PREVIEW)
    {
        // Pré-description affichée en haut : on attend la confirmation
        const install_info_t *info = installer_preview_info();
        uint64_t free_b = installer_free_space();
        bool fits = !(info->size && free_b && info->size > free_b);

        gfx_text_wrap(c, &font_small, 10, 52, SCREEN_W - 20, 40,
                      fits ? "Vérifie les informations de l'écran du haut, puis confirme."
                           : "Il n'y a pas assez de place sur la carte SD pour ce fichier.",
                      fits ? COL_TEXT_DIM : COL_ERR);
        if (ui_button(c, RECT(BTN_X0, 98, BTN_W, BTN_H), "Installer", BTN_STYLE_PRIMARY,
                      fits) ||
            (fits && (in->down & BTN_START)))
            installer_confirm();
        if (ui_button(c, RECT(BTN_X0, 134, BTN_W, BTN_H), "Annuler", BTN_STYLE_NORMAL,
                      true) ||
            (in->down & BTN_B))
            installer_cancel();
        return;
    }

    gfx_text_box(c, &font_small, 10, 50, SCREEN_W - 20, ALIGN_LEFT,
                 "Lien GitHub ou URL d'un .nds / .dsi / .zip", COL_TEXT_DIM);

    // Le champ est un widget : on le touche (ou A) pour ouvrir le clavier
    rect_t f = RECT(8, 64, SCREEN_W - 16, 26);
    bool pressed, focused;
    if (ui_hit(f, !busy, &pressed, &focused))
    {
        kbd_open(app.url, sizeof(app.url));
        go(SCREEN_URL);
    }
    draw_text_field(c, f, app.url, "github.com/auteur/projet", false,
                    focused || pressed);

    if (busy)
    {
        char now[16], total[24], line[64];
        format_size(now, sizeof(now), installer_progress_now());
        if (installer_progress_total() > 0)
        {
            format_size(total, sizeof(total), installer_progress_total());
            snprintf(line, sizeof(line), "%s / %s", now, total);
        }
        else
        {
            snprintf(line, sizeof(line), "%s", now);
        }

        draw_progress_bar(c, RECT(16, 100, SCREEN_W - 32, 10),
                          installer_progress_now(), installer_progress_total());
        int lw = gfx_text_width(&font_small, line);
        gfx_text_box(c, &font_small, 16, 114, SCREEN_W - 32 - lw - 8, ALIGN_LEFT,
                     installer_message(), COL_TEXT);
        gfx_text(c, &font_small, SCREEN_W - 16 - lw, 114, line, COL_TEXT_DIM);

        // Mode détaillé : débit et durée restante du téléchargement
        bool verbose = installer_verbose();
        if (verbose && installer_state() == INST_DOWNLOADING)
        {
            char speed[24], eta[32], detail[80];
            uint64_t now_b = installer_progress_now(), total_b = installer_progress_total();
            instinfo_format_speed(speed, sizeof(speed), installer_speed());
            instinfo_format_eta(eta, sizeof(eta), total_b > now_b ? total_b - now_b : 0,
                                installer_speed());
            if (speed[0] && total_b > 0)
                snprintf(detail, sizeof(detail), "%s · reste %s", speed, eta);
            else if (speed[0])
                snprintf(detail, sizeof(detail), "%s", speed);
            else
                snprintf(detail, sizeof(detail), "Mesure du débit…");
            gfx_text_box(c, &font_small, 16, 128, SCREEN_W - 32, ALIGN_LEFT, detail,
                         COL_TEXT_DIM);
        }

        if (ui_button(c, RECT(BTN_X0, verbose ? 148 : 140, BTN_W, BTN_H), "Annuler",
                      BTN_STYLE_DANGER, true) ||
            (in->down & BTN_B))
            installer_cancel();
    }
    else
    {
        bool can = online && installer_storage_ok() && app.url[0] != '\0';
        const int half = (BTN_W - 6) / 2;
        if (ui_button(c, RECT(BTN_X0, 98, half, BTN_H), "Installer",
                      BTN_STYLE_PRIMARY, can) ||
            (can && (in->down & BTN_START)))
        {
            if (!installer_start(app.url))
                ui_toast("Impossible de lancer l'installation");
        }

        // Parcourir les projets d'un utilisateur GitHub
        if (ui_button(c, RECT(BTN_X0 + half + 6, 98, half, BTN_H), "Parcourir",
                      BTN_STYLE_NORMAL, online && installer_storage_ok()))
        {
            // Dernière recherche encore en mémoire : on la ré-affiche tout de suite
            if (installer_browse_state() == BROWSE_DONE)
            {
                go(SCREEN_BROWSE);
            }
            else
            {
                kbd_open(app.browse_user, sizeof(app.browse_user));
                go(SCREEN_BROWSE_USER);
            }
        }

        if (ui_button(c, RECT(BTN_X0, 98 + BTN_H + 6, BTN_W, BTN_H), "Retour",
                      BTN_STYLE_NORMAL, true) ||
            (in->down & BTN_B))
            go(SCREEN_HOME);

        const char *warn = NULL;
        if (!installer_storage_ok())
            warn = "Carte SD introuvable";
        else if (!online)
            warn = "Connecte-toi d'abord au Wi-Fi";
        if (warn)
            gfx_text_box(c, &font_small, 0, SCREEN_H - 16, SCREEN_W, ALIGN_CENTER,
                         warn, COL_WARN);
        else
        {
            char dest[96];
            snprintf(dest, sizeof(dest), "Destination : %s", installer_dir());
            gfx_text_box(c, &font_small, 0, SCREEN_H - 16, SCREEN_W, ALIGN_CENTER,
                         dest, COL_TEXT_OFF);
        }
    }
}

static void back_to_games(void)
{
    refresh_games();
    app.install_tab = app.sel_from_sd ? TAB_SD : TAB_GAMES;
    if (app.sel_from_sd)
        sd_enter();       // Ré-analyse seulement si la carte a changé
    go(SCREEN_INSTALL);
}

static void screen_game(canvas_t *c, const input_t *in)
{
    const game_t *g = &app.sel;
    ui_header(c, g->file, NULL);

    bool busy = install_busy() || installer_checking();
    bool online = net_state() == NET_CONNECTED;
    bool tracked = games_is_tracked(g);
    bool in_shop_dir = strcmp(app.sel_dir, installer_dir()) == 0;

    // Ligne d'état de la mise à jour
    const char *latest;
    upd_status_t st = installer_update_status(g->file, &latest);
    char line[256];
    uint16_t col = COL_TEXT_DIM;
    if (!in_shop_dir)
        snprintf(line, sizeof(line), "Dossier : %s", app.sel_dir);
    else if (!tracked)
        snprintf(line, sizeof(line), "Installé à la main : pas de mise à jour auto");
    else if (st == UPD_AVAILABLE)
    {
        snprintf(line, sizeof(line), "Nouvelle version disponible%s%s",
                 latest[0] ? " : " : "", latest);
        col = COL_WARN;
    }
    else if (st == UPD_UP_TO_DATE)
    {
        snprintf(line, sizeof(line), "À jour%s%s", g->version[0] ? " : " : "", g->version);
        col = COL_OK;
    }
    else
        snprintf(line, sizeof(line), "Version installée : %s",
                 g->version[0] ? g->version : "inconnue");
    gfx_text_box(c, &font_small, 10, 27, SCREEN_W - 20, ALIGN_LEFT, line, col);

    const int x = BTN_X0, w = BTN_W, h = 24, gap = 4;
    int y = 44;

    const int half = (w - gap) / 2;
    if (ui_button(c, RECT(x, y, half, h), "Lancer", BTN_STYLE_PRIMARY,
                  installer_twl_present() && !busy))
    {
        const char *err = installer_launch_game(app.sel_dir, g->file);
        if (err)
            ui_toast(err);
        else
        {
            app.launch_frame = app.frame;
            go(SCREEN_LAUNCH);
        }
        return;
    }
    if (ui_button(c, RECT(x + half + gap, y, half, h), "Mettre à jour", BTN_STYLE_NORMAL,
                  tracked && in_shop_dir && online && !busy))
    {
        if (!installer_update(g->file, false))
            ui_toast("Impossible de lancer la mise à jour");
        return;
    }
    y += h + gap;

    if (ui_button(c, RECT(x, y, w, h), "Revenir à la version précédente",
                  BTN_STYLE_NORMAL, app.sel_backup && !busy))
    {
        app.confirm = CONFIRM_RESTORE;
        go(SCREEN_CONFIRM);
        return;
    }
    y += h + gap;

    char saves[48];
    snprintf(saves, sizeof(saves), "Supprimer les sauvegardes (%d)", app.sel_saves);
    if (ui_button(c, RECT(x, y, w, h), saves, BTN_STYLE_DANGER,
                  app.sel_saves > 0 && !busy))
    {
        app.confirm = CONFIRM_DELETE_SAVES;
        go(SCREEN_CONFIRM);
        return;
    }
    y += h + gap;

    if (ui_button(c, RECT(x, y, w, h), in_shop_dir ? "Désinstaller" : "Supprimer le jeu",
                  BTN_STYLE_DANGER, !busy))
    {
        app.confirm = CONFIRM_UNINSTALL;
        go(SCREEN_CONFIRM);
        return;
    }
    y += h + gap;

    if (ui_button(c, RECT(x, y, w, h), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        back_to_games();
}

// Compte à rebours avant le redémarrage qui lance le jeu : on peut encore annuler
static void screen_launch(canvas_t *c, const input_t *in)
{
    ui_header(c, "Lancement", NULL);

    int left = LAUNCH_DELAY_FRAMES - (int)(app.frame - app.launch_frame);
    ui_spinner(c, SCREEN_W / 2, 56, 8, app.frame, COL_ACCENT);
    char line[96];
    snprintf(line, sizeof(line), "Redémarrage dans %d s…", left > 0 ? (left + 59) / 60 : 0);
    gfx_text_box(c, &font_body, 8, 74, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT);

    if (ui_button(c, RECT(BTN_X0, 110, BTN_W, BTN_H), "Annuler", BTN_STYLE_DANGER, left > 0) ||
        ((in->down & BTN_B) && left > 0))
    {
        installer_launch_restore();   // Remet le réglage de TWiLight d'avant
        go(SCREEN_GAME);
        return;
    }
    if (left <= 0)
        app.reboot = true;
}

static void screen_confirm(canvas_t *c, const input_t *in)
{
    const game_t *g = &app.sel;
    const char *title = "", *text = "", *action = "";
    btn_style_t style = BTN_STYLE_DANGER;

    switch (app.confirm)
    {
        case CONFIRM_UNINSTALL:
            title = "Désinstaller";
            text = "Le jeu sera supprimé de la carte SD. Ses sauvegardes sont "
                   "gardées : tu les retrouveras si tu le réinstalles.";
            action = "Désinstaller";
            break;
        case CONFIRM_DELETE_SAVES:
            title = "Supprimer les sauvegardes";
            text = "Toutes les sauvegardes de ce jeu seront effacées. Utile si "
                   "le jeu plante après une mise à jour. C'est définitif !";
            action = "Effacer les sauvegardes";
            break;
        case CONFIRM_RESTORE:
            title = "Version précédente";
            text = "Le jeu revient à la version d'avant la dernière mise à jour. "
                   "Les sauvegardes ne sont pas touchées.";
            action = "Revenir en arrière";
            style = BTN_STYLE_PRIMARY;
            break;
    }

    ui_header(c, title, NULL);
    gfx_text_box(c, &font_body, 12, 32, SCREEN_W - 24, ALIGN_CENTER, g->file, COL_TEXT);
    gfx_text_wrap(c, &font_small, 20, 54, SCREEN_W - 40, 50, text, COL_TEXT_DIM);

    if (ui_button(c, RECT(BTN_X0, 106, BTN_W, BTN_H), action, style, !install_busy()))
    {
        const char *dir = app.sel_dir;
        char msg[96];
        switch (app.confirm)
        {
            case CONFIRM_UNINSTALL:
                if (games_uninstall(dir, g->file))
                {
                    app.sd_dirty = true;
                    back_to_games();
                    ui_toast("Jeu désinstallé");
                }
                else
                    ui_toast("Impossible de supprimer le fichier");
                return;

            case CONFIRM_DELETE_SAVES:
            {
                int n = games_delete_saves(dir, g->file);
                snprintf(msg, sizeof(msg), "%d sauvegarde(s) supprimée(s)", n);
                reload_selected();
                go(SCREEN_GAME);
                ui_toast(msg);
                return;
            }

            case CONFIRM_RESTORE:
                if (games_restore_backup(dir, g->file))
                {
                    refresh_games();
                    reload_selected();
                    go(SCREEN_GAME);
                    ui_toast("Version précédente restaurée");
                }
                else
                    ui_toast("Impossible de restaurer la version précédente");
                return;
        }
    }

    if (ui_button(c, RECT(BTN_X0, 106 + BTN_H + 6, BTN_W, BTN_H), "Annuler",
                  BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_GAME);
}

static void screen_url(canvas_t *c, const input_t *in)
{
    ui_header(c, "Lien du jeu", NULL);

    gfx_text_box(c, &font_small, 10, 28, SCREEN_W - 20, ALIGN_LEFT,
                 "Ex. : github.com/auteur/projet", COL_TEXT_DIM);

    rect_t f = RECT(8, 42, SCREEN_W - 16, 26);
    draw_text_field(c, f, app.url, NULL, true, true);

    kbd_result_t r = kbd_update(c, in, 88);
    if (r != KBD_EDITING)
        go(SCREEN_INSTALL);
}

// Saisie du nom d'utilisateur GitHub, puis chargement de ses projets
static void screen_browse_user(canvas_t *c, const input_t *in)
{
    ui_header(c, "Parcourir GitHub", NULL);

    const char *const tabs[2] = { "Jeu (recherche)", "Utilisateur" };
    int tab = ui_tabs(c, RECT(8, 26, SCREEN_W - 16, 18), tabs, 2, app.browse_mode);
    if (tab != app.browse_mode)
    {
        app.browse_mode = tab;
        app.force_redraw = true;
    }

    rect_t f = RECT(8, 48, SCREEN_W - 16, 26);
    draw_text_field(c, f, app.browse_user,
                    app.browse_mode == BMODE_SEARCH ? "Nom du jeu (ex. : pokemon)"
                                                    : "Utilisateur (ex. : WiIIiam278)",
                    true, true);

    kbd_result_t r = kbd_update(c, in, 88);
    if (r == KBD_EDITING)
        return;

    if (r == KBD_ACCEPTED && app.browse_user[0] != '\0')
    {
        bool ok = app.browse_mode == BMODE_SEARCH
                      ? installer_browse_search(app.browse_user)
                      : installer_browse_start(app.browse_user);
        if (ok)
        {
            app.browse_scroll = 0;
            go(SCREEN_BROWSE);
            return;
        }
        ui_toast("Saisie invalide ou occupé");
    }
    go(SCREEN_INSTALL);
}

#define BROWSE_Y     30
#define BROWSE_ROW_H 32
#define BROWSE_ROWS  4

static void screen_browse(canvas_t *c, const input_t *in)
{
    char title[64];
    if (installer_browse_is_search())
        snprintf(title, sizeof(title), "Jeux : %s", installer_browse_user());
    else
        snprintf(title, sizeof(title), "Projets de %s", installer_browse_user());
    ui_header(c, title, NULL);

    browse_state_t st = installer_browse_state();
    int count = installer_browse_count();

    if (st == BROWSE_LOADING)
    {
        gfx_text_box(c, &font_body, 8, 80, SCREEN_W - 16, ALIGN_CENTER,
                     "Chargement des projets…", COL_TEXT);
    }
    else if (st == BROWSE_ERROR)
    {
        gfx_text_wrap(c, &font_small, 16, 70, SCREEN_W - 32, 70,
                      installer_browse_message(), COL_ERR);
    }
    else if (st == BROWSE_DONE)
    {
        int max_scroll = count > BROWSE_ROWS ? count - BROWSE_ROWS : 0;
        if (in->repeat & BTN_L)
            app.browse_scroll--;
        if (in->repeat & BTN_R)
            app.browse_scroll++;
        if (app.browse_scroll > max_scroll)
            app.browse_scroll = max_scroll;
        if (app.browse_scroll < 0)
            app.browse_scroll = 0;

        for (int row = 0; row < BROWSE_ROWS; row++)
        {
            int i = app.browse_scroll + row;
            if (i >= count)
                break;
            const browse_repo_t *rp = installer_browse_get(i);

            char stars[16];
            snprintf(stars, sizeof(stars), "* %d", rp->stars);
            rect_t r = RECT(8, BROWSE_Y + row * (BROWSE_ROW_H + 3), SCREEN_W - 16 - 12,
                            BROWSE_ROW_H);
            char shown[112];
            if (installer_browse_is_search())
                snprintf(shown, sizeof(shown), "%s/%s", rp->owner, rp->name);
            else
                snprintf(shown, sizeof(shown), "%s", rp->name);
            if (ui_list_item(c, r, shown, rp->desc, stars, COL_TEXT_DIM))
            {
                // Installe la dernière release de ce projet
                char link[160];
                snprintf(link, sizeof(link), "%s/%s", rp->owner, rp->name);
                if (installer_start(link))
                {
                    snprintf(app.url, sizeof(app.url), "github.com/%s", link);
                    app.install_tab = TAB_INSTALL;
                    go(SCREEN_INSTALL);
                }
                else
                    ui_toast("Impossible de lancer l'installation");
                return;
            }
        }

        if (count > BROWSE_ROWS)
        {
            int track_h = BROWSE_ROWS * (BROWSE_ROW_H + 3) - 3;
            int thumb_h = track_h * BROWSE_ROWS / count;
            int thumb_y = BROWSE_Y + (track_h - thumb_h) * app.browse_scroll / max_scroll;
            gfx_fill_round_rect(c, SCREEN_W - 12, BROWSE_Y, 4, track_h, 2, COL_PANEL);
            gfx_fill_round_rect(c, SCREEN_W - 12, thumb_y, 4, thumb_h, 2, COL_ACCENT);
        }
    }

    int by = SCREEN_H - 28;
    if (ui_button(c, RECT(8, by, 30, 22), "\xE2\x86\x91", BTN_STYLE_NORMAL,
                  st == BROWSE_DONE && app.browse_scroll > 0))
        app.browse_scroll--;
    if (ui_button(c, RECT(42, by, 104, 22), "Autre recherche", BTN_STYLE_NORMAL,
                  st != BROWSE_LOADING))
    {
        kbd_open(app.browse_user, sizeof(app.browse_user));
        go(SCREEN_BROWSE_USER);
    }
    if (ui_button(c, RECT(150, by, 64, 22), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
    {
        if (st == BROWSE_LOADING)
            installer_cancel();
        go(SCREEN_INSTALL);
    }
    if (ui_button(c, RECT(218, by, 30, 22), "\xE2\x86\x93", BTN_STYLE_NORMAL,
                  st == BROWSE_DONE && app.browse_scroll < count - BROWSE_ROWS))
        app.browse_scroll++;
}

static int reboot_seconds_left(void)
{
    unsigned int elapsed = app.frame - app.done_frame;
    if (elapsed >= REBOOT_DELAY_FRAMES)
        return 0;
    return (int)((REBOOT_DELAY_FRAMES - elapsed + 59) / 60);
}

static void screen_install_done(canvas_t *c, const input_t *in)
{
    ui_header(c, installer_last_was_update() ? "Mise à jour terminée"
                                             : "Installation terminée", NULL);

    gfx_fill_circle(c, SCREEN_W / 2, 50, 14, COL_OK);
    draw_check(c, SCREEN_W / 2 - 1, 50, COL_BG);

    gfx_text_box(c, &font_body, 12, 70, SCREEN_W - 24, ALIGN_CENTER,
                 installer_file_name(), COL_TEXT);

    char line[48];
    snprintf(line, sizeof(line), "Redémarrage dans %d s…", reboot_seconds_left());
    gfx_text_box(c, &font_small, 0, 88, SCREEN_W, ALIGN_CENTER, line, COL_TEXT_DIM);

    if (ui_button(c, RECT(BTN_X0, 108, BTN_W, BTN_H), "Redémarrer maintenant",
                  BTN_STYLE_PRIMARY, true))
        app.reboot = true;

    if (ui_button(c, RECT(BTN_X0, 108 + BTN_H + 6, BTN_W, BTN_H),
                  "Plus tard (installer un autre jeu)", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
    {
        go(SCREEN_INSTALL);
        return;
    }

    if (reboot_seconds_left() == 0)
        app.reboot = true;
}

// Mise à jour de TWiLight Menu++ (paquets servis par le serveur du catalogue)
static void screen_twl(canvas_t *c, const input_t *in)
{
    ui_header(c, "TWiLight Menu++", NULL);

    twl_state_t st = installer_twl_state();
    bool online = net_state() == NET_CONNECTED;
    const int by = 104;   // Bouton principal
    bool back = true;     // Bouton Retour affiché
    char line[160];

    if (!installer_twl_present())
    {
        gfx_text_wrap(c, &font_small, 16, 40, SCREEN_W - 32, 60,
                      "TWiLight Menu++ n'a pas été trouvé sur la carte SD de la "
                      "console : rien à mettre à jour.", COL_TEXT_DIM);
    }
    else if (st == TWL_CHECKING)
    {
        ui_spinner(c, SCREEN_W / 2, 56, 8, app.frame, COL_ACCENT);
        gfx_text_box(c, &font_body, 8, 74, SCREEN_W - 16, ALIGN_CENTER,
                     "Recherche de la dernière version…", COL_TEXT);
    }
    else if (st == TWL_UPDATING)
    {
        int pack, packs;
        uint64_t now, total;
        char a[24], b[24];
        installer_twl_progress(&pack, &packs, &now, &total);
        snprintf(line, sizeof(line), "Paquet %d sur %d", pack, packs);
        gfx_text_box(c, &font_body, 8, 40, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT);
        draw_progress_bar(c, RECT(BTN_X0, 64, BTN_W, 10), now, total ? total : 1);
        format_size(a, sizeof(a), now);
        format_size(b, sizeof(b), total);
        snprintf(line, sizeof(line), "%s / %s", a, b);
        gfx_text_box(c, &font_small, 8, 80, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT_DIM);

        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), "Annuler", BTN_STYLE_DANGER, true))
            installer_cancel();
        gfx_text_box(c, &font_small, 0, SCREEN_H - 20, SCREEN_W, ALIGN_CENTER,
                     "N'éteins pas la console", COL_WARN);
        back = false;
    }
    else if (st == TWL_DONE)
    {
        draw_check(c, SCREEN_W / 2, 48, COL_OK);
        snprintf(line, sizeof(line), "TWiLight Menu++ %s installé", installer_twl_latest());
        gfx_text_box(c, &font_body, 8, 64, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT);
        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), "Redémarrer la console",
                      BTN_STYLE_PRIMARY, true))
            app.reboot = true;
    }
    else if (st == TWL_READY)
    {
        const char *have = installer_twl_installed(), *latest = installer_twl_latest();
        bool same = strcmp(have, latest) == 0;
        int resume = installer_twl_resume();

        snprintf(line, sizeof(line), "Installée : %s", have[0] ? have : "inconnue");
        gfx_text_box(c, &font_body, 8, 36, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT);
        snprintf(line, sizeof(line), "Disponible : %s", latest);
        gfx_text_box(c, &font_body, 8, 36 + font_body.height + 2, SCREEN_W - 16,
                     ALIGN_CENTER, line, same && !resume ? COL_TEXT : COL_OK);
        if (resume)
            gfx_text_box(c, &font_small, 8, 80, SCREEN_W - 16, ALIGN_CENTER,
                         "Une mise à jour a été interrompue", COL_WARN);
        else if (same)
            gfx_text_box(c, &font_small, 8, 80, SCREEN_W - 16, ALIGN_CENTER,
                         "TWiLight Menu++ est à jour", COL_TEXT_DIM);

        const char *label = resume ? "Reprendre la mise à jour"
                          : same ? "Réinstaller cette version" : "Mettre à jour";
        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), label,
                      same && !resume ? BTN_STYLE_NORMAL : BTN_STYLE_PRIMARY, online))
        {
            if (!installer_twl_update())
                ui_toast("Impossible de lancer la mise à jour");
        }
    }
    else
    {
        // Pas encore vérifié, ou erreur
        if (st == TWL_ERROR)
            gfx_text_wrap(c, &font_small, 16, 34, SCREEN_W - 32, 66,
                          installer_twl_message(), COL_ERR);
        else
            gfx_text_wrap(c, &font_small, 16, 40, SCREEN_W - 32, 60,
                          "Vérifie s'il existe une nouvelle version de TWiLight "
                          "Menu++ et installe-la sur la carte SD.", COL_TEXT_DIM);

        bool retry_update = st == TWL_ERROR && installer_twl_latest()[0] != '\0';
        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H),
                      retry_update ? "Reprendre la mise à jour"
                                   : st == TWL_ERROR ? "Réessayer" : "Vérifier",
                      BTN_STYLE_PRIMARY, online))
        {
            bool ok = retry_update ? installer_twl_update() : installer_twl_check();
            if (!ok)
                ui_toast("Impossible de lancer la mise à jour");
        }
    }

    if (back)
    {
        if (ui_button(c, RECT(BTN_X0, by + BTN_H + 6, BTN_W, BTN_H), "Retour",
                      BTN_STYLE_NORMAL, true) ||
            (in->down & BTN_B))
            go(SCREEN_MORE);
        if (!online && installer_twl_present() && st != TWL_DONE)
            gfx_text_box(c, &font_small, 0, SCREEN_H - 16, SCREEN_W, ALIGN_CENTER,
                         "Connecte-toi d'abord au Wi-Fi", COL_WARN);
    }
}

static void screen_about(canvas_t *c, const input_t *in)
{
    ui_header(c, "À propos", NULL);

    gfx_text_box(c, &font_title, 0, 30, SCREEN_W, ALIGN_CENTER, APP_NAME, COL_TEXT);
    gfx_text_box(c, &font_small, 0, 30 + font_title.height, SCREEN_W, ALIGN_CENTER,
                 "Version " APP_VERSION, COL_TEXT_DIM);
    // Carte SD introuvable : on montre pourquoi (à me communiquer pour corriger)
    if (!installer_storage_ok())
        gfx_text_wrap(c, &font_small, 8, SCREEN_H - 30, SCREEN_W - 16, 28,
                      installer_storage_info(), COL_WARN);

    // Mise à jour de DSi Shop lui-même (dernière release GitHub du projet)
    self_state_t st = installer_self_state();
    bool online = net_state() == NET_CONNECTED;
    bool can = online && installer_self_available();
    const int ty = 66, by = 102;
    char line[160];

    if (st == SELF_UPDATING)
    {
        uint64_t now = installer_progress_now(), total = installer_progress_total();
        snprintf(line, sizeof(line), "Téléchargement de %s…", installer_self_latest());
        gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER, line, COL_TEXT);
        draw_progress_bar(c, RECT(BTN_X0, ty + 18, BTN_W, 10), now, total);
        if (ui_button(c, RECT(BTN_X0, by + BTN_H + 6, BTN_W, BTN_H), "Annuler",
                      BTN_STYLE_DANGER, true))
            installer_cancel();
        return;
    }

    if (st == SELF_CHECKING)
    {
        gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER,
                     "Recherche d'une nouvelle version…", COL_TEXT);
    }
    else if (st == SELF_DONE)
    {
        snprintf(line, sizeof(line), "%s installée : redémarre pour l'utiliser",
                 installer_self_latest());
        gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER, line, COL_OK);
        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), "Redémarrer la console",
                      BTN_STYLE_PRIMARY, true))
            app.reboot = true;
    }
    else if (st == SELF_READY && installer_self_newer())
    {
        snprintf(line, sizeof(line), "Nouvelle version disponible : %s",
                 installer_self_latest());
        gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER, line, COL_OK);
        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), "Mettre à jour DSi Shop",
                      BTN_STYLE_PRIMARY, can))
        {
            if (!installer_self_update())
                ui_toast("Impossible de lancer la mise à jour");
        }
    }
    else
    {
        if (st == SELF_READY)
            gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER,
                         "DSi Shop est à jour", COL_TEXT_DIM);
        else if (st == SELF_ERROR)
            gfx_text_wrap(c, &font_small, 16, ty - 6, SCREEN_W - 32, 38,
                          installer_self_message(), COL_ERR);
        else if (!installer_self_available())
            gfx_text_wrap(c, &font_small, 16, ty - 6, SCREEN_W - 32, 38,
                          "Mise à jour impossible : DSi Shop ne sait pas où il est "
                          "rangé sur la carte SD.", COL_TEXT_DIM);
        else if (!online)
            gfx_text_box(c, &font_small, 8, ty, SCREEN_W - 16, ALIGN_CENTER,
                         "Connecte-toi au Wi-Fi pour chercher une mise à jour", COL_WARN);

        if (ui_button(c, RECT(BTN_X0, by, BTN_W, BTN_H), "Rechercher une mise à jour",
                      BTN_STYLE_NORMAL, can && st != SELF_CHECKING))
        {
            if (!installer_self_check(APP_VERSION))
                ui_toast("Impossible de lancer la recherche");
        }
    }

    if (ui_button(c, RECT(BTN_X0, by + BTN_H + 6, BTN_W, BTN_H), "Retour",
                  BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_MORE);
}

// ---------------------------------------------------------------------------

// Réagit aux installations (locales ou lancées depuis le Remote) et aux
// événements du Remote, quel que soit l'écran affiché
static void handle_background_events(void)
{
    inst_state_t st = installer_state();
    if (st != app.last_inst_state)
    {
        bool busy = st >= INST_RESOLVING && st <= INST_VERIFYING;
        bool was_busy = app.last_inst_state >= INST_RESOLVING &&
                        app.last_inst_state <= INST_VERIFYING;

        if (busy && !was_busy && app.screen != SCREEN_INSTALL)
        {
            app.install_tab = TAB_INSTALL;
            go(SCREEN_INSTALL);
            if (installer_was_remote())
                ui_toast(installer_last_was_update() ? "Mise à jour demandée depuis le Remote"
                                                     : "Installation demandée depuis le Remote");
        }
        else if (st == INST_DONE && installer_last_was_update() && installer_up_to_date())
        {
            // Rien de nouveau : on retourne à la fiche du jeu
            refresh_games();
            char msg[160];
            snprintf(msg, sizeof(msg), "%s est déjà à jour", installer_file_name());
            if (!installer_was_remote() && strcmp(app.sel.file, installer_file_name()) == 0)
            {
                reload_selected();
                go(SCREEN_GAME);
            }
            ui_toast(msg);
        }
        else if (st == INST_DONE)
        {
            refresh_games();
            app.sd_dirty = true;
            if (installer_was_remote())
            {
                // Pas de redémarrage automatique : on peut en enchaîner plusieurs
                char msg[160];
                snprintf(msg, sizeof(msg), "%s : %s",
                         installer_last_was_update() ? "Mis à jour" : "Installé",
                         installer_file_name());
                ui_toast(msg);
            }
            else
            {
                app.done_frame = app.frame;
                go(SCREEN_INSTALL_DONE);
            }
        }
        else if (st == INST_ERROR)
        {
            ui_toast(installer_last_was_update() ? "Échec de la mise à jour"
                                                 : "Échec de l'installation");
        }
        app.last_inst_state = st;
    }

    remote_event_t ev;
    while ((ev = remote_poll_event()) != REMOTE_EV_NONE ||
           (ev = lremote_glue_poll_event()) != REMOTE_EV_NONE)
    {
        switch (ev)
        {
            case REMOTE_EV_GAMES_CHANGED:
                refresh_games();
                app.sd_dirty = true;
                ui_toast("Jeu désinstallé depuis le Remote");
                app.force_redraw = true;
                break;
            case REMOTE_EV_REBOOT:
                app.reboot = true;
                break;
            case REMOTE_EV_PAIRED:
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "Remote : connecté à %s", remote_owner_name());
                ui_toast(msg);
                break;
            }
            default:
                break;
        }
    }
}

void app_frame(const input_t *in)
{
    app.frame++;

    handle_background_events();
    sd_scan_tick();

    ui_begin(in);

    // Écran du bas : on ne le redessine que s'il y a des entrées, une
    // animation, ou si l'état a changé (voir top_signature)
    uint32_t sig = top_signature();
    bool state_changed = sig != app.top_signature;
    bool bottom_needed = app.force_redraw || state_changed || ui_wants_redraw() ||
                         in->down || in->up || in->repeat ||
                         app.screen == SCREEN_PASSWORD || // Curseur clignotant
                         app.screen == SCREEN_URL || app.screen == SCREEN_LR_EDIT ||
                         app.screen == SCREEN_BROWSE_USER ||
                         (app.screen == SCREEN_BROWSE &&
                          installer_browse_state() == BROWSE_LOADING) ||
                         app.screen == SCREEN_INSTALL_DONE || app.screen == SCREEN_LAUNCH ||
                         (app.screen == SCREEN_INSTALL && install_busy()) ||
                         (app.screen == SCREEN_TWL && twl_busy()) ||
                         (app.screen == SCREEN_INSTALL && app.install_tab == TAB_SD &&
                          app.sd_started && !app.sdscan.done) ||
                         (app.screen == SCREEN_ABOUT && self_busy()) ||
                         (app.screen == SCREEN_REMOTE && remote_pair_url()[0] == '\0');

    app_screen_t drawn = app.screen;
    app.force_redraw = false;

    if (bottom_needed)
    {
        canvas_t *c = gfx_canvas(SCREEN_BOTTOM);
        gfx_clear(c, COL_BG);

        switch (app.screen)
        {
            case SCREEN_HOME:     screen_home(c, in); break;
            case SCREEN_WIFI:     screen_wifi(c, in); break;
            case SCREEN_SCAN:     screen_scan(c, in); break;
            case SCREEN_PASSWORD: screen_password(c, in); break;
            case SCREEN_HTTP:     screen_http(c, in); break;
            case SCREEN_ABOUT:    screen_about(c, in); break;
            case SCREEN_INSTALL:  screen_install(c, in); break;
            case SCREEN_URL:      screen_url(c, in); break;
            case SCREEN_INSTALL_DONE: screen_install_done(c, in); break;
            case SCREEN_GAME:     screen_game(c, in); break;
            case SCREEN_CONFIRM:  screen_confirm(c, in); break;
            case SCREEN_REMOTE:   screen_remote(c, in); break;
            case SCREEN_LR_EDIT:  screen_lr_edit(c, in); break;
            case SCREEN_MORE:     screen_more(c, in); break;
            case SCREEN_BROWSE_USER: screen_browse_user(c, in); break;
            case SCREEN_BROWSE:   screen_browse(c, in); break;
            case SCREEN_TWL:      screen_twl(c, in); break;
            case SCREEN_SETTINGS: screen_settings(c, in); break;
            case SCREEN_LAUNCH:   screen_launch(c, in); break;
        }

        ui_draw_toast(c);
    }

    ui_end();

    // Si l'écran a changé pendant cette frame, on redessine tout à la suivante
    if (drawn != app.screen)
        app.force_redraw = true;

    sig = top_signature();
    if (sig != app.top_signature || app.force_redraw)
    {
        app.top_signature = sig;
        draw_top(gfx_canvas(SCREEN_TOP));
    }
}
