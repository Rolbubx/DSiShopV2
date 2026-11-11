// Logique de l'application : écrans, navigation et rendu

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "gfx.h"
#include "installer.h"
#include "keyboard.h"
#include "net.h"
#include "ui.h"

// Serveur utilisé pour le test HTTP (texte clair, pas de HTTPS)
#define TEST_HOST "example.com"
#define TEST_PATH "/"

typedef enum {
    SCREEN_HOME,
    SCREEN_WIFI,
    SCREEN_SCAN,
    SCREEN_PASSWORD,
    SCREEN_HTTP,
    SCREEN_ABOUT,
    SCREEN_INSTALL,       // Saisie du lien + progression
    SCREEN_URL,           // Clavier pour taper le lien
    SCREEN_INSTALL_DONE,  // Installé : compte à rebours avant redémarrage
} app_screen_t;

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

    if (app.wifi_ok)
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
        {
            // Titre "hero"
            gfx_text_box(c, &font_title, 0, 36, SCREEN_W, ALIGN_CENTER,
                         "Bienvenue sur " APP_NAME, COL_TEXT);
            gfx_text_box(c, &font_small, 0, 36 + font_title.height + 2, SCREEN_W,
                         ALIGN_CENTER, "Utilise l'écran tactile ou la croix + A",
                         COL_TEXT_DIM);
            draw_net_card(c, RECT(12, 86, SCREEN_W - 24, 94));
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
            gfx_text(c, &font_title, 12, 30, "C'est installé !", COL_OK);
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
    ui_header(c, "Accueil", APP_VERSION);

    bool online = net_state() == NET_CONNECTED;

    int y = 32;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Installer un jeu",
                  BTN_STYLE_PRIMARY, true))
        go(SCREEN_INSTALL);
    y += BTN_H + 6;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H),
                  online ? "Wi-Fi (connecté)" : "Wi-Fi", BTN_STYLE_NORMAL,
                  app.wifi_ok))
        go(SCREEN_WIFI);
    y += BTN_H + 6;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Test Internet (HTTP)",
                  BTN_STYLE_NORMAL, online))
        go(SCREEN_HTTP);
    y += BTN_H + 6;

    int half = (BTN_W - 8) / 2;
    if (ui_button(c, RECT(BTN_X0, y, half, BTN_H), "À propos", BTN_STYLE_NORMAL, true))
        go(SCREEN_ABOUT);
    if (ui_button(c, RECT(BTN_X0 + half + 8, y, half, BTN_H), "Quitter",
                  BTN_STYLE_DANGER, true))
        app.quit = true;

    if (!app.wifi_ok)
        gfx_text_box(c, &font_small, 0, SCREEN_H - 14, SCREEN_W, ALIGN_CENTER,
                     "Le Wi-Fi n'a pas pu être initialisé", COL_ERR);
    else if (!installer_storage_ok())
        gfx_text_box(c, &font_small, 0, SCREEN_H - 14, SCREEN_W, ALIGN_CENTER,
                     "Carte SD introuvable : installation impossible", COL_ERR);
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
        go(SCREEN_HOME);

    if (!online)
        gfx_text_box(c, &font_small, 0, SCREEN_H - 22, SCREEN_W, ALIGN_CENTER,
                     "Connecte-toi d'abord au Wi-Fi", COL_WARN);
}

static void screen_install(canvas_t *c, const input_t *in)
{
    ui_header(c, "Installer un jeu", NULL);

    bool busy = install_busy();
    bool online = net_state() == NET_CONNECTED;

    gfx_text_box(c, &font_small, 10, 28, SCREEN_W - 20, ALIGN_LEFT,
                 "Lien GitHub ou URL d'un .nds / .dsi / .zip", COL_TEXT_DIM);

    // Le champ est un widget : on le touche (ou A) pour ouvrir le clavier
    rect_t f = RECT(8, 42, SCREEN_W - 16, 26);
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

        draw_progress_bar(c, RECT(16, 80, SCREEN_W - 32, 10),
                          installer_progress_now(), installer_progress_total());
        int lw = gfx_text_width(&font_small, line);
        gfx_text_box(c, &font_small, 16, 94, SCREEN_W - 32 - lw - 8, ALIGN_LEFT,
                     installer_message(), COL_TEXT);
        gfx_text(c, &font_small, SCREEN_W - 16 - lw, 94, line, COL_TEXT_DIM);

        if (ui_button(c, RECT(BTN_X0, 120, BTN_W, BTN_H), "Annuler",
                      BTN_STYLE_DANGER, true) ||
            (in->down & BTN_B))
            installer_cancel();
    }
    else
    {
        bool can = online && installer_storage_ok() && app.url[0] != '\0';
        if (ui_button(c, RECT(BTN_X0, 80, BTN_W, BTN_H), "Installer",
                      BTN_STYLE_PRIMARY, can) ||
            (can && (in->down & BTN_START)))
        {
            if (!installer_start(app.url))
                ui_toast("Impossible de lancer l'installation");
        }

        if (ui_button(c, RECT(BTN_X0, 80 + BTN_H + 6, BTN_W, BTN_H), "Retour",
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

    // Fin d'installation : écran de redémarrage, ou message d'erreur
    inst_state_t st = installer_state();
    if (st != app.last_inst_state)
    {
        if (st == INST_DONE)
        {
            app.done_frame = app.frame;
            go(SCREEN_INSTALL_DONE);
        }
        else if (st == INST_ERROR)
        {
            ui_toast("Échec de l'installation");
        }
        app.last_inst_state = st;
    }
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
    ui_header(c, "Installation terminée", NULL);

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
        go(SCREEN_HOME);
}

// ---------------------------------------------------------------------------

void app_frame(const input_t *in)
{
    app.frame++;

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
                         (app.screen == SCREEN_INSTALL && install_busy());

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
