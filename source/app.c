// Logique de l'application : écrans, navigation et rendu

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "gfx.h"
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
} app_screen_t;

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

    // Pour ne redessiner l'écran du haut que si quelque chose a changé
    uint32_t top_signature;
    bool force_redraw;
} app;

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
    go(SCREEN_HOME);
}

bool app_should_quit(void)
{
    return app.quit;
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

static bool net_busy(void)
{
    net_state_t s = net_state();
    return s == NET_CONNECTING || s == NET_SCANNING || http_state() == HTTP_BUSY;
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

    int y = 34;
    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Wi-Fi", BTN_STYLE_PRIMARY,
                  app.wifi_ok))
        go(SCREEN_WIFI);
    y += BTN_H + 8;

    if (ui_button(c, RECT(BTN_X0, y, BTN_W, BTN_H), "Test Internet (HTTP)",
                  BTN_STYLE_NORMAL, net_state() == NET_CONNECTED))
        go(SCREEN_HTTP);
    y += BTN_H + 8;

    int half = (BTN_W - 8) / 2;
    if (ui_button(c, RECT(BTN_X0, y, half, BTN_H), "À propos", BTN_STYLE_NORMAL, true))
        go(SCREEN_ABOUT);
    if (ui_button(c, RECT(BTN_X0 + half + 8, y, half, BTN_H), "Quitter",
                  BTN_STYLE_DANGER, true))
        app.quit = true;

    if (!app.wifi_ok)
        gfx_text_box(c, &font_small, 0, SCREEN_H - 22, SCREEN_W, ALIGN_CENTER,
                     "Le Wi-Fi n'a pas pu être initialisé", COL_ERR);
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

    // Champ de saisie
    rect_t f = RECT(8, 42, SCREEN_W - 16, 26);
    gfx_fill_round_rect(c, f.x, f.y, f.w, f.h, 5, COL_ACCENT);
    gfx_fill_round_rect(c, f.x + 1, f.y + 1, f.w - 2, f.h - 2, 4, COL_BG);

    // Affiche la fin du mot de passe si trop long
    const char *shown = app.password;
    while (gfx_text_width(&font_body, shown) > f.w - 20)
        shown++;
    int ty = f.y + (f.h - font_body.height) / 2;
    int end = gfx_text(c, &font_body, f.x + 8, ty, shown, COL_TEXT);
    if ((app.frame / 30) & 1)
        gfx_vline(c, end + 1, ty + 1, font_body.height - 2, COL_ACCENT);

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
                         app.screen == SCREEN_PASSWORD; // Curseur clignotant

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
