// Logique de l'application : écrans, navigation et rendu

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "games.h"
#include "gfx.h"
#include "installer.h"
#include "keyboard.h"
#include "net.h"
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
    SCREEN_MORE,          // Test Internet, À propos, Quitter
} app_screen_t;

// Actions qui demandent une confirmation
typedef enum {
    CONFIRM_UNINSTALL,
    CONFIRM_DELETE_SAVES,
    CONFIRM_RESTORE,
} confirm_t;

// Onglets de l'écran "Installer un jeu"
enum { TAB_INSTALL, TAB_GAMES };

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

    bool reboot;
    unsigned int done_frame;
    inst_state_t last_inst_state;

    // Jeux installés
    int install_tab;
    game_t games[GAMES_MAX];
    int ngames;
    bool games_loaded;
    int games_scroll;
    game_t sel;           // Jeu affiché dans la fiche
    int sel_saves;        // Nombre de fichiers de sauvegarde trouvés
    bool sel_backup;      // Une version précédente est disponible
    confirm_t confirm;

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

static bool net_busy(void)
{
    net_state_t s = net_state();
    return s == NET_CONNECTING || s == NET_SCANNING || http_state() == HTTP_BUSY ||
           install_busy();
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

static void icon_qr(canvas_t *c, int cx, int cy, uint16_t col, uint16_t bg)
{
    // Trois "yeux" de QR code et quelques modules
    const int s = 3;
    const int eyes[3][2] = { { -15, -15 }, { 6, -15 }, { -15, 6 } };
    for (int i = 0; i < 3; i++)
    {
        int x = cx + eyes[i][0], y = cy + eyes[i][1];
        gfx_fill_rect(c, x, y, 3 * s, 3 * s, col);
        gfx_fill_rect(c, x + 2, y + 2, 3 * s - 4, 3 * s - 4, bg);
        gfx_fill_rect(c, x + 3, y + 3, 3, 3, col);
    }
    static const int8_t dots[][2] = {
        { 1, 1 }, { 2, 3 }, { 4, 2 }, { 1, 5 }, { 3, 6 }, { 5, 4 }, { 6, 6 },
        { 0, 3 }, { 5, 0 }, { 6, 2 },
    };
    for (size_t i = 0; i < sizeof(dots) / sizeof(dots[0]); i++)
        gfx_fill_rect(c, cx + dots[i][0] * s - 1, cy + dots[i][1] * s - 1, s, s, col);
}

typedef enum { ICON_DOWNLOAD, ICON_QR } icon_t;

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

    int dy = pressed ? 1 : 0;
    int cx = r.x + r.w / 2, cy = r.y + 30 + dy;
    if (icon == ICON_DOWNLOAD)
        icon_download(c, cx, cy, COL_TEXT);
    else
        icon_qr(c, cx, cy, COL_TEXT, bg);

    gfx_text_box(c, &font_body, r.x + 4, r.y + 54 + dy, r.w - 8, ALIGN_CENTER,
                 title, COL_TEXT);
    gfx_text_box(c, &font_small, r.x + 4, r.y + 54 + font_body.height + dy, r.w - 8,
                 ALIGN_CENTER, sub, gfx_blend(bg, COL_TEXT, 11));
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
                     i == current ? COL_TEXT : COL_TEXT_DIM);
    }
    return current;
}

// Texte de statut du Remote pour l'accueil
static void remote_summary(char *out, size_t size)
{
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

static void draw_top(canvas_t *c)
{
    gfx_gradient_v(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG2, COL_BG);
    draw_top_header(c);

    switch (app.screen)
    {
        case SCREEN_HOME:
        case SCREEN_MORE:
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
            uint16_t dot = remote_state() == REMOTE_PAIRED ? COL_OK : COL_TEXT_OFF;
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

        case SCREEN_REMOTE:
        {
            gfx_text(c, &font_title, 12, 30, "Remote", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);
            int x = r.x + 8, y = r.y + 6;

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
        {
            gfx_text(c, &font_title, 12, 30, "Installer un jeu", COL_TEXT);
            rect_t r = RECT(8, 54, SCREEN_W - 16, SCREEN_H - 62);
            ui_panel(c, r);

            inst_state_t st = installer_state();

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

            // Gros fichiers : stockés en morceaux de 1 Go (limite FAT32 : 4 Go)
            const uint64_t GB = 1024ull * 1024 * 1024;
            uint64_t total = installer_progress_total();
            if (st == INST_DOWNLOADING && total > GB)
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
    MIX(installer_progress_now() / 8192);
    MIX(installer_progress_total());
    MIX(strlen(installer_message()));
    MIX((unsigned char)installer_message()[0]);
    if (app.screen == SCREEN_INSTALL_DONE)
        MIX(reboot_seconds_left());
    MIX(time(NULL) / 60);
    MIX(installer_checking());
    if (installer_checking())
    {
        int done, total;
        installer_check_progress(&done, &total);
        MIX(done);
    }
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

    // Les deux grandes tuiles
    if (ui_tile(c, RECT(8, 30, 116, 96), "Installer un jeu", "et gérer mes jeux",
                ICON_DOWNLOAD, true))
    {
        app.install_tab = TAB_INSTALL;
        go(SCREEN_INSTALL);
    }

    const char *rsub = remote_state() == REMOTE_PAIRED ? "Connecté"
                                                        : "QR code · téléphone";
    if (ui_tile(c, RECT(132, 30, 116, 96), "Remote", rsub, ICON_QR, false))
    {
        remote_start();
        remote_request_pair_code();
        go(SCREEN_REMOTE);
    }

    // Raccourcis
    char games[32];
    snprintf(games, sizeof(games), "Jeux (%d)", app.ngames);
    const int bw = (SCREEN_W - 16 - 2 * 6) / 3;
    if (ui_button(c, RECT(8, 134, bw, 26), games, BTN_STYLE_NORMAL, true))
    {
        app.install_tab = TAB_GAMES;
        refresh_games();
        go(SCREEN_INSTALL);
    }
    if (ui_button(c, RECT(8 + bw + 6, 134, bw, 26),
                  net_state() == NET_CONNECTED ? "Wi-Fi \xE2\x80\xA2" : "Wi-Fi",
                  BTN_STYLE_NORMAL, app.wifi_ok))
        go(SCREEN_WIFI);
    if (ui_button(c, RECT(8 + (bw + 6) * 2, 134, bw, 26), "Plus…", BTN_STYLE_NORMAL,
                  true))
        go(SCREEN_MORE);

    const char *warn = NULL;
    if (!app.wifi_ok)
        warn = "Le Wi-Fi n'a pas pu être initialisé";
    else if (!installer_storage_ok())
        warn = "Carte SD introuvable : installation impossible";
    if (warn)
    {
        gfx_text_box(c, &font_small, 0, SCREEN_H - 20, SCREEN_W, ALIGN_CENTER, warn,
                     COL_ERR);
    }
    else
    {
        char line[96];
        snprintf(line, sizeof(line), "Wi-Fi : %s", net_state_text());
        gfx_text_box(c, &font_small, 0, SCREEN_H - 20, SCREEN_W, ALIGN_CENTER, line,
                     COL_TEXT_OFF);
    }
}

static void screen_more(canvas_t *c, const input_t *in)
{
    ui_header(c, "Plus", NULL);

    int y = 32;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Test Internet (HTTP)",
                  BTN_STYLE_NORMAL, net_state() == NET_CONNECTED))
        go(SCREEN_HTTP);
    y += BTN_H + 6;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "À propos", BTN_STYLE_NORMAL, true))
        go(SCREEN_ABOUT);
    y += BTN_H + 6;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Quitter DSi Shop",
                  BTN_STYLE_DANGER, true))
        app.quit = true;
    y += BTN_H + 6;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Retour", BTN_STYLE_NORMAL, true) ||
        (in->down & BTN_B))
        go(SCREEN_HOME);
}

static void screen_remote(canvas_t *c, const input_t *in)
{
    ui_header(c, "Remote", NULL);

    remote_state_t rs = remote_state();
    const int col_x = 150, col_w = SCREEN_W - col_x - 8;
    bool back = false;

    if (rs == REMOTE_NO_SERVER)
    {
        gfx_text_wrap(c, &font_small, 16, 34, SCREEN_W - 32, 100,
                      "Aucun serveur Remote n'est configuré. Regarde l'écran du "
                      "haut pour savoir comment faire.", COL_TEXT_DIM);
        back = ui_button(c, RECT(BTN_X0, 150, BTN_W, BTN_H), "Retour",
                         BTN_STYLE_NORMAL, true);
    }
    else
    {
        rect_t qr = RECT(6, 26, 140, 140);
        const char *url = remote_pair_url();

        if (url[0])
        {
            draw_qr(c, qr, url);
            gfx_text_box(c, &font_small, col_x, 30, col_w, ALIGN_LEFT,
                         rs == REMOTE_PAIRED ? "Associer un autre :" : "Ou tape le code :",
                         COL_TEXT_DIM);
            gfx_text_box(c, &font_body, col_x, 32 + font_small.height, col_w,
                         ALIGN_LEFT, remote_pair_code(), COL_TEXT);
        }
        else
        {
            gfx_fill_round_rect(c, qr.x + 10, qr.y + 10, qr.w - 20, qr.h - 20, 8,
                                COL_PANEL);
            if (rs == REMOTE_WAIT_WIFI)
                gfx_text_wrap(c, &font_small, qr.x + 20, qr.y + 50, qr.w - 40, 60,
                              "Connecte-toi d'abord au Wi-Fi", COL_TEXT_DIM);
            else if (rs == REMOTE_ERROR)
                gfx_text_wrap(c, &font_small, qr.x + 20, qr.y + 40, qr.w - 40, 80,
                              remote_status_text(), COL_ERR);
            else
                ui_spinner(c, qr.x + qr.w / 2, qr.y + qr.h / 2, 8, app.frame,
                           COL_ACCENT);
        }

        // Statut sous le code
        char line[96];
        if (rs == REMOTE_PAIRED)
            snprintf(line, sizeof(line), "Connecté à %s", remote_owner_name());
        else
            snprintf(line, sizeof(line), "%s", remote_status_text());
        gfx_text_wrap(c, &font_small, col_x, 72, col_w, font_small.height * 3, line,
                      rs == REMOTE_PAIRED ? COL_OK : COL_TEXT_DIM);

        if (rs == REMOTE_WAIT_WIFI)
        {
            if (ui_button(c, RECT(col_x, 112, col_w, 24), "Wi-Fi", BTN_STYLE_PRIMARY,
                          app.wifi_ok))
                go(SCREEN_WIFI);
        }
        else if (rs == REMOTE_PAIRED)
        {
            if (ui_button(c, RECT(col_x, 112, col_w, 24), "Délier", BTN_STYLE_DANGER,
                          true))
            {
                remote_forget();
                ui_toast("DSi déliée : un nouveau QR code arrive");
            }
        }
        else
        {
            if (ui_button(c, RECT(col_x, 112, col_w, 24), "Nouveau code",
                          BTN_STYLE_NORMAL, rs == REMOTE_WAIT_PAIR))
                remote_request_pair_code();
        }

        back = ui_button(c, RECT(col_x, 142, col_w, 24), "Retour", BTN_STYLE_NORMAL,
                         true);
    }

    if (back || (in->down & BTN_B))
        go(SCREEN_HOME);
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

// Ouvre la fiche d'un jeu (recalcule sauvegardes et version précédente)
static void open_game(const game_t *g)
{
    app.sel = *g;
    app.sel_saves = games_find_saves(installer_dir(), g->file, NULL, 0);
    app.sel_backup = games_has_backup(installer_dir(), g->file);
    go(SCREEN_GAME);
}

// Recharge la fiche du jeu affiché (après une mise à jour par exemple)
static void reload_selected(void)
{
    for (int i = 0; i < app.ngames; i++)
        if (strcmp(app.games[i].file, app.sel.file) == 0)
            app.sel = app.games[i];
    app.sel_saves = games_find_saves(installer_dir(), app.sel.file, NULL, 0);
    app.sel_backup = games_has_backup(installer_dir(), app.sel.file);
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

static void screen_install(canvas_t *c, const input_t *in)
{
    ui_header(c, "Installer un jeu", NULL);

    char games_label[32];
    snprintf(games_label, sizeof(games_label), "Mes jeux (%d)", app.ngames);
    const char *const tabs[2] = { "Installer", games_label };
    int tab = ui_tabs(c, RECT(8, 26, SCREEN_W - 16, 20), tabs, 2, app.install_tab);
    if (tab != app.install_tab)
    {
        app.install_tab = tab;
        if (tab == TAB_GAMES)
            refresh_games();
        app.force_redraw = true;
    }

    if (app.install_tab == TAB_GAMES)
    {
        screen_games(c, in);
        return;
    }

    bool busy = install_busy();
    bool online = net_state() == NET_CONNECTED;

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

        if (ui_button(c, RECT(BTN_X0, 140, BTN_W, BTN_H), "Annuler",
                      BTN_STYLE_DANGER, true) ||
            (in->down & BTN_B))
            installer_cancel();
    }
    else
    {
        bool can = online && installer_storage_ok() && app.url[0] != '\0';
        if (ui_button(c, RECT(BTN_X0, 98, BTN_W, BTN_H), "Installer",
                      BTN_STYLE_PRIMARY, can) ||
            (can && (in->down & BTN_START)))
        {
            if (!installer_start(app.url))
                ui_toast("Impossible de lancer l'installation");
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
    app.install_tab = TAB_GAMES;
    go(SCREEN_INSTALL);
}

static void screen_game(canvas_t *c, const input_t *in)
{
    const game_t *g = &app.sel;
    ui_header(c, g->file, NULL);

    bool busy = install_busy() || installer_checking();
    bool online = net_state() == NET_CONNECTED;
    bool tracked = games_is_tracked(g);

    // Ligne d'état de la mise à jour
    const char *latest;
    upd_status_t st = installer_update_status(g->file, &latest);
    char line[128];
    uint16_t col = COL_TEXT_DIM;
    if (!tracked)
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

    if (ui_button(c, RECT(x, y, w, h), "Mettre à jour", BTN_STYLE_PRIMARY,
                  tracked && online && !busy))
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

    if (ui_button(c, RECT(x, y, w, h), "Désinstaller", BTN_STYLE_DANGER, !busy))
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
        const char *dir = installer_dir();
        char msg[96];
        switch (app.confirm)
        {
            case CONFIRM_UNINSTALL:
                if (games_uninstall(dir, g->file))
                {
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

static void screen_about(canvas_t *c, const input_t *in)
{
    ui_header(c, "À propos", NULL);

    gfx_text_box(c, &font_title, 0, 50, SCREEN_W, ALIGN_CENTER, APP_NAME, COL_TEXT);
    gfx_text_box(c, &font_small, 0, 50 + font_title.height, SCREEN_W, ALIGN_CENTER,
                 "Version " APP_VERSION, COL_TEXT_DIM);

    if (ui_button(c, RECT(BTN_X0, 120, BTN_W, BTN_H), "Retour", BTN_STYLE_NORMAL,
                  true) ||
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
    while ((ev = remote_poll_event()) != REMOTE_EV_NONE)
    {
        switch (ev)
        {
            case REMOTE_EV_GAMES_CHANGED:
                refresh_games();
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

    ui_begin(in);

    // Écran du bas : on ne le redessine que s'il y a des entrées, une
    // animation, ou si l'état a changé (voir top_signature)
    uint32_t sig = top_signature();
    bool state_changed = sig != app.top_signature;
    bool bottom_needed = app.force_redraw || state_changed || ui_wants_redraw() ||
                         in->down || in->up || in->repeat ||
                         app.screen == SCREEN_PASSWORD || // Curseur clignotant
                         app.screen == SCREEN_URL ||
                         app.screen == SCREEN_INSTALL_DONE ||
                         (app.screen == SCREEN_INSTALL && install_busy()) ||
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
            case SCREEN_MORE:     screen_more(c, in); break;
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
