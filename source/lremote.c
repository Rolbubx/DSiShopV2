// Remote local : identifiants, sessions, requêtes HTTP (voir lremote.h)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "lremote.h"
#include "lremote_page.h"
#include "sha256.h"

#define HASH_ROUNDS     2000            // Ralentit les essais de mot de passe
#define MAX_SESSIONS    8
#define SESSION_TTL     (12 * 3600)     // 12 h
#define MAX_REQUEST     (8 * 1024)
#define MAX_BODY        (4 * 1024)
#define LOCK_AFTER      5               // Essais ratés avant blocage
#define LOCK_BASE_S     30              // Blocage : 30 s, puis double (max 5 min)

static const lremote_backend_t *be;

static struct {
    char path[256];
    char user[LREMOTE_USER_MAX + 1];
    uint8_t salt[16];
    uint8_t hash[32];
    bool configured;
    bool enabled;
} cfg;

static struct {
    char token[65];     // 32 octets en hexadécimal
    time_t expires;
} sessions[MAX_SESSIONS];

static int failures;
static time_t locked_until;
static int lock_level;

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = '\0';
}

static bool from_hex(const char *in, uint8_t *out, size_t n)
{
    if (strlen(in) != n * 2)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        unsigned v;
        if (sscanf(in + i * 2, "%2x", &v) != 1)
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// Comparaison en temps constant (ne révèle pas où ça diffère)
static bool same_bytes(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

static void hash_password(const char *pass, const uint8_t salt[16], uint8_t out[32])
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt, 16);
    sha256_update(&c, pass, strlen(pass));
    sha256_final(&c, out);
    for (int i = 0; i < HASH_ROUNDS; i++)
    {
        sha256_init(&c);
        sha256_update(&c, out, 32);
        sha256_update(&c, salt, 16);
        sha256_update(&c, pass, strlen(pass));
        sha256_final(&c, out);
    }
}

static bool valid_user(const char *u)
{
    size_t n = strlen(u);
    if (n < LREMOTE_USER_MIN || n > LREMOTE_USER_MAX)
        return false;
    for (; *u; u++)
        if ((unsigned char)*u < 0x21 || *u == 0x7F) // Pas d'espace ni de caractère de contrôle
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static void save_config(void)
{
    if (!cfg.path[0])
        return;
    FILE *f = fopen(cfg.path, "w");
    if (!f)
        return;
    if (cfg.configured)
    {
        char salt[33], hash[65];
        to_hex(cfg.salt, 16, salt);
        to_hex(cfg.hash, 32, hash);
        fprintf(f, "user\t%s\nsalt\t%s\nhash\t%s\n", cfg.user, salt, hash);
    }
    fprintf(f, "enabled\t%d\n", cfg.enabled ? 1 : 0);
    fclose(f);
}

static void load_config(void)
{
    cfg.configured = false;
    cfg.enabled = false;
    FILE *f = cfg.path[0] ? fopen(cfg.path, "r") : NULL;
    if (!f)
        return;

    char line[160];
    bool has_salt = false, has_hash = false;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *v = strchr(line, '\t');
        if (!v)
            continue;
        *v++ = '\0';
        if (strcmp(line, "user") == 0 && valid_user(v))
            snprintf(cfg.user, sizeof(cfg.user), "%s", v);
        else if (strcmp(line, "salt") == 0)
            has_salt = from_hex(v, cfg.salt, 16);
        else if (strcmp(line, "hash") == 0)
            has_hash = from_hex(v, cfg.hash, 32);
        else if (strcmp(line, "enabled") == 0)
            cfg.enabled = atoi(v) != 0;
    }
    fclose(f);
    cfg.configured = cfg.user[0] && has_salt && has_hash;
}

void lremote_init(const lremote_backend_t *backend, const char *config_path)
{
    be = backend;
    if (strlen(config_path) >= sizeof(cfg.path))
        config_path = ""; // Trop long : refusé plutôt que tronqué
    snprintf(cfg.path, sizeof(cfg.path), "%s", config_path);
    load_config();
    memset(sessions, 0, sizeof(sessions));
}

bool lremote_configured(void) { return cfg.configured; }
const char *lremote_user(void) { return cfg.configured ? cfg.user : ""; }
bool lremote_enabled(void) { return cfg.enabled && cfg.configured; }

void lremote_set_enabled(bool enabled)
{
    cfg.enabled = enabled;
    save_config();
}

bool lremote_set_credentials(const char *user, const char *pass, char *err,
                             size_t err_size)
{
    if (!valid_user(user))
    {
        snprintf(err, err_size, "Identifiant : %d à %d caractères, sans espace",
                 LREMOTE_USER_MIN, LREMOTE_USER_MAX);
        return false;
    }
    size_t n = strlen(pass);
    if (n < LREMOTE_PASS_MIN || n > LREMOTE_PASS_MAX)
    {
        snprintf(err, err_size, "Mot de passe : %d à %d caractères",
                 LREMOTE_PASS_MIN, LREMOTE_PASS_MAX);
        return false;
    }

    snprintf(cfg.user, sizeof(cfg.user), "%s", user);
    be->random(cfg.salt, sizeof(cfg.salt));
    hash_password(pass, cfg.salt, cfg.hash);
    cfg.configured = true;
    cfg.enabled = true;
    save_config();

    // Les anciennes sessions ne sont plus valables
    memset(sessions, 0, sizeof(sessions));
    failures = 0;
    locked_until = 0;
    lock_level = 0;
    return true;
}

void lremote_clear_credentials(void)
{
    memset(&cfg.salt, 0, sizeof(cfg.salt));
    memset(&cfg.hash, 0, sizeof(cfg.hash));
    cfg.user[0] = '\0';
    cfg.configured = false;
    cfg.enabled = false;
    save_config();
    memset(sessions, 0, sizeof(sessions));
}

int lremote_sessions(void)
{
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].token[0] && sessions[i].expires > now)
            n++;
    return n;
}

// ---------------------------------------------------------------------------
// Construction des réponses
// ---------------------------------------------------------------------------

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} sb_t;

static void sb_add(sb_t *sb, const char *s, size_t n)
{
    if (sb->oom)
        return;
    if (sb->len + n + 1 > sb->cap)
    {
        size_t cap = sb->cap ? sb->cap : 1024;
        while (sb->len + n + 1 > cap)
            cap *= 2;
        char *nb = realloc(sb->buf, cap);
        if (!nb)
        {
            sb->oom = true;
            return;
        }
        sb->buf = nb;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(sb_t *sb, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_add(sb, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

// Chaîne JSON échappée (avec les guillemets)
static void sb_json(sb_t *sb, const char *s)
{
    sb_add(sb, "\"", 1);
    for (; s && *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            char e[2] = { '\\', (char)c };
            sb_add(sb, e, 2);
        }
        else if (c < 0x20)
        {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            sb_add(sb, e, 6);
        }
        else
        {
            sb_add(sb, (const char *)&c, 1);
        }
    }
    sb_add(sb, "\"", 1);
}

static char *response(int code, const char *status, const char *type,
                      const char *extra_headers, const char *body, size_t body_len,
                      size_t *out_len)
{
    sb_t sb = { 0 };
    sb_printf(&sb,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %u\r\n"
              "Cache-Control: no-store\r\n"
              "X-Content-Type-Options: nosniff\r\n"
              "X-Frame-Options: DENY\r\n"
              "Referrer-Policy: no-referrer\r\n"
              "Connection: close\r\n",
              code, status, type, (unsigned)body_len);
    if (extra_headers)
        sb_add(&sb, extra_headers, strlen(extra_headers));
    sb_add(&sb, "\r\n", 2);
    sb_add(&sb, body, body_len);
    if (sb.oom)
    {
        free(sb.buf);
        return NULL;
    }
    *out_len = sb.len;
    return sb.buf;
}

static char *text_reply(int code, const char *status, const char *text, size_t *out_len)
{
    return response(code, status, "text/plain; charset=utf-8", NULL, text, strlen(text),
                    out_len);
}

static char *json_reply(int code, const char *status, const char *json,
                        const char *extra_headers, size_t *out_len)
{
    return response(code, status, "application/json; charset=utf-8", extra_headers,
                    json, strlen(json), out_len);
}

static char *json_error(int code, const char *status, const char *msg, size_t *out_len)
{
    sb_t sb = { 0 };
    sb_add(&sb, "{\"error\":", 9);
    sb_json(&sb, msg);
    sb_add(&sb, "}", 1);
    char *r = json_reply(code, status, sb.buf ? sb.buf : "{}", NULL, out_len);
    free(sb.buf);
    return r;
}

// ---------------------------------------------------------------------------
// Analyse des requêtes
// ---------------------------------------------------------------------------

typedef struct {
    char method[8];
    char path[128];
    char host[64];
    char cookie_token[65];
    bool csrf_ok;          // En-tête X-DSiShop: 1 présent
    const char *body;
    size_t body_len;
} request_t;

static const char *find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    return NULL;
}

// Valeur d'un en-tête (insensible à la casse), copiée dans out
static bool header_value(const char *headers, const char *end, const char *name,
                         char *out, size_t size)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p < end)
    {
        const char *eol = p;
        while (eol < end && *eol != '\r')
            eol++;
        if ((size_t)(eol - p) > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':')
        {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            size_t n = (size_t)(eol - v);
            if (n >= size)
                n = size - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        p = eol + 2;
    }
    return false;
}

long lremote_request_length(const char *buf, size_t len)
{
    const char *end = find_header_end(buf, len);
    if (!end)
        return len >= MAX_REQUEST ? -1 : 0;

    size_t head = (size_t)(end - buf) + 4;
    char cl[16] = "";
    long body = 0;
    if (header_value(buf, end, "Content-Length", cl, sizeof(cl)))
    {
        body = strtol(cl, NULL, 10);
        if (body < 0 || body > MAX_BODY)
            return -1;
    }
    if (head + (size_t)body > MAX_REQUEST)
        return -1;
    return len >= head + (size_t)body ? (long)(head + body) : 0;
}

static bool parse_request(const char *buf, size_t len, request_t *r)
{
    memset(r, 0, sizeof(*r));
    const char *end = find_header_end(buf, len);
    if (!end)
        return false;

    // Ligne de requête : MÉTHODE CHEMIN HTTP/1.x
    if (sscanf(buf, "%7s %127s", r->method, r->path) != 2)
        return false;
    char *q = strchr(r->path, '?');
    if (q)
        *q = '\0';

    const char *headers = strstr(buf, "\r\n");
    if (!headers || headers > end)
        return false;
    headers += 2;

    header_value(headers, end, "Host", r->host, sizeof(r->host));

    char v[256];
    if (header_value(headers, end, "X-DSiShop", v, sizeof(v)))
        r->csrf_ok = strcmp(v, "1") == 0;

    if (header_value(headers, end, "Cookie", v, sizeof(v)))
    {
        const char *t = strstr(v, "dsishop_local=");
        if (t)
        {
            t += strlen("dsishop_local=");
            size_t n = strspn(t, "0123456789abcdef");
            if (n == 64)
            {
                memcpy(r->cookie_token, t, 64);
                r->cookie_token[64] = '\0';
            }
        }
    }

    r->body = end + 4;
    r->body_len = len - (size_t)(r->body - buf);
    return true;
}

// Seules les adresses IP sont acceptées comme nom d'hôte (et "localhost" pour
// les tests) : bloque les attaques par "DNS rebinding" depuis un site web
static bool host_allowed(const char *host)
{
    char h[64];
    snprintf(h, sizeof(h), "%s", host);
    char *colon = strchr(h, ':');
    if (colon)
        *colon = '\0';
    if (strcmp(h, "localhost") == 0)
        return true;
    if (!h[0])
        return false;
    int dots = 0;
    for (const char *p = h; *p; p++)
    {
        if (*p == '.')
            dots++;
        else if (*p < '0' || *p > '9')
            return false;
    }
    return dots == 3;
}

// Extrait une chaîne d'un petit objet JSON ({"user":"...","pass":"..."})
static bool json_get(const char *body, size_t len, const char *key, char *out,
                     size_t size)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    static char tmp[MAX_BODY + 1]; // Statique : trop gros pour la pile de la DS
    if (len > MAX_BODY)
        len = MAX_BODY;
    memcpy(tmp, body, len);
    tmp[len] = '\0';

    const char *p = strstr(tmp, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return false;
    p++;

    size_t n = 0;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\')
        {
            char e = *p++;
            if (e == 'u')
            {
                unsigned cp = 0;
                if (sscanf(p, "%4x", &cp) != 1)
                    return false;
                p += 4;
                // Encodage UTF-8 (sans les paires de substitution)
                char u[3];
                int ul = 0;
                if (cp < 0x80)       { u[ul++] = (char)cp; }
                else if (cp < 0x800) { u[ul++] = (char)(0xC0 | (cp >> 6)); u[ul++] = (char)(0x80 | (cp & 63)); }
                else { u[ul++] = (char)(0xE0 | (cp >> 12)); u[ul++] = (char)(0x80 | ((cp >> 6) & 63)); u[ul++] = (char)(0x80 | (cp & 63)); }
                for (int i = 0; i < ul && n + 1 < size; i++)
                    out[n++] = u[i];
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            if (!e)
                return false;
        }
        if (n + 1 < size)
            out[n++] = c;
        else
            return false; // Trop long : refusé plutôt que tronqué
    }
    out[n] = '\0';
    return *p == '"';
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

static bool session_valid(const char *token)
{
    if (!token[0])
        return false;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].token[0] && sessions[i].expires > now &&
            same_bytes(sessions[i].token, token, 64))
            return true;
    }
    return false;
}

static const char *session_create(void)
{
    time_t now = time(NULL);
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        // Place libre ou expirée, sinon la plus ancienne
        if (!sessions[i].token[0] || sessions[i].expires <= now)
        {
            slot = i;
            break;
        }
        if (sessions[i].expires < sessions[slot].expires)
            slot = i;
    }
    uint8_t raw[32];
    be->random(raw, sizeof(raw));
    to_hex(raw, sizeof(raw), sessions[slot].token);
    sessions[slot].expires = now + SESSION_TTL;
    return sessions[slot].token;
}

static void session_delete(const char *token)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].token[0] && same_bytes(sessions[i].token, token, 64))
            memset(&sessions[i], 0, sizeof(sessions[i]));
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

static char *serve_static(const char *path, size_t *out_len)
{
    static const char *csp =
        "Content-Security-Policy: default-src 'self'; "
        "connect-src 'self' https://api.github.com; img-src 'self' data:; "
        "style-src 'self'; script-src 'self'; frame-ancestors 'none'\r\n";

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
        return response(200, "OK", "text/html; charset=utf-8", csp, lremote_index_html,
                        strlen(lremote_index_html), out_len);
    if (strcmp(path, "/app.js") == 0)
        return response(200, "OK", "text/javascript; charset=utf-8", NULL, lremote_app_js,
                        strlen(lremote_app_js), out_len);
    if (strcmp(path, "/style.css") == 0)
        return response(200, "OK", "text/css; charset=utf-8", NULL, lremote_style_css,
                        strlen(lremote_style_css), out_len);
    return text_reply(404, "Not Found", "Introuvable", out_len);
}

static char *route_login(const request_t *r, size_t *out_len)
{
    time_t now = time(NULL);
    if (now < locked_until)
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Trop d'essais : réessaie dans %d s",
                 (int)(locked_until - now));
        return json_error(429, "Too Many Requests", msg, out_len);
    }

    char user[LREMOTE_USER_MAX + 2], pass[LREMOTE_PASS_MAX + 2];
    bool ok = json_get(r->body, r->body_len, "user", user, sizeof(user)) &&
              json_get(r->body, r->body_len, "pass", pass, sizeof(pass));

    // Toujours calculer l'empreinte, même si l'identifiant est faux : même durée
    uint8_t h[32];
    hash_password(ok ? pass : "", cfg.salt, h);
    bool user_ok = ok && strlen(user) == strlen(cfg.user) &&
                   same_bytes(user, cfg.user, strlen(cfg.user));
    bool pass_ok = same_bytes(h, cfg.hash, 32);
    memset(pass, 0, sizeof(pass));

    if (!(user_ok && pass_ok))
    {
        failures++;
        if (failures >= LOCK_AFTER)
        {
            int secs = LOCK_BASE_S << (lock_level < 4 ? lock_level : 4);
            if (secs > 300)
                secs = 300;
            locked_until = now + secs;
            lock_level++;
            failures = 0;
        }
        return json_error(401, "Unauthorized", "Identifiant ou mot de passe incorrect",
                          out_len);
    }

    failures = 0;
    lock_level = 0;
    const char *token = session_create();
    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "Set-Cookie: dsishop_local=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d\r\n",
             token, SESSION_TTL);

    sb_t sb = { 0 };
    sb_add(&sb, "{\"user\":", 8);
    sb_json(&sb, cfg.user);
    sb_add(&sb, "}", 1);
    char *res = json_reply(200, "OK", sb.buf ? sb.buf : "{}", cookie, out_len);
    free(sb.buf);
    return res;
}

static game_t status_games[GAMES_MAX];

static char *route_status(size_t *out_len)
{
    rstatus_t st;
    memset(&st, 0, sizeof(st));
    be->fill_status(&st);
    int n = be->list_games(status_games, GAMES_MAX);

    sb_t sb = { 0 };
    sb_add(&sb, "{\"user\":", 8);
    sb_json(&sb, cfg.user);
    sb_add(&sb, ",\"state\":", 9);
    sb_json(&sb, st.state);
    sb_add(&sb, ",\"message\":", 11);
    sb_json(&sb, st.message);
    sb_add(&sb, ",\"file\":", 8);
    sb_json(&sb, st.file);
    sb_add(&sb, ",\"release\":", 11);
    sb_json(&sb, st.release);
    sb_add(&sb, ",\"version\":", 11);
    sb_json(&sb, st.version);
    sb_printf(&sb, ",\"progress\":{\"now\":%llu,\"total\":%llu},\"free\":%llu,\"games\":[",
              (unsigned long long)st.now, (unsigned long long)st.total,
              (unsigned long long)st.free_bytes);
    for (int i = 0; i < n; i++)
    {
        const game_t *g = &status_games[i];
        sb_add(&sb, i ? ",{\"file\":" : "{\"file\":", i ? 9 : 8);
        sb_json(&sb, g->file);
        sb_printf(&sb, ",\"size\":%llu,\"source\":", (unsigned long long)g->size);
        sb_json(&sb, g->source);
        sb_add(&sb, ",\"version\":", 11);
        sb_json(&sb, g->version);
        sb_add(&sb, ",\"url\":", 7);
        sb_json(&sb, g->url);
        sb_add(&sb, ",\"update\":", 10);
        sb_json(&sb, be->update_status ? be->update_status(g->file) : "");
        sb_add(&sb, "}", 1);
    }
    sb_add(&sb, "]}", 2);

    char *res = sb.oom ? NULL : json_reply(200, "OK", sb.buf, NULL, out_len);
    free(sb.buf);
    return res;
}

static char *route_command(const request_t *r, size_t *out_len)
{
    char type[16], arg[320] = "";
    if (!json_get(r->body, r->body_len, "type", type, sizeof(type)))
        return json_error(400, "Bad Request", "Commande invalide", out_len);
    json_get(r->body, r->body_len, "arg", arg, sizeof(arg));

    bool valid = false;
    if (strcmp(type, "install") == 0)
        valid = arg[0] && strpbrk(arg, " \t\r\n") == NULL;
    else if (strcmp(type, "update") == 0 || strcmp(type, "uninstall") == 0)
        valid = games_valid_name(arg);
    else if (strcmp(type, "cancel") == 0 || strcmp(type, "reboot") == 0 ||
             strcmp(type, "check") == 0)
        valid = true;
    if (!valid)
        return json_error(400, "Bad Request", "Commande invalide", out_len);

    char msg[160] = "";
    bool ok = be->command(type, arg, msg, sizeof(msg));

    sb_t sb = { 0 };
    sb_printf(&sb, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    sb_json(&sb, msg);
    sb_add(&sb, "}", 1);
    char *res = sb.oom ? NULL : json_reply(ok ? 200 : 409, ok ? "OK" : "Conflict",
                                           sb.buf, NULL, out_len);
    free(sb.buf);
    return res;
}

char *lremote_handle(const char *req, size_t len, size_t *out_len)
{
    request_t r;
    if (!parse_request(req, len, &r))
        return text_reply(400, "Bad Request", "Requête invalide", out_len);

    if (!host_allowed(r.host))
        return text_reply(403, "Forbidden", "Utilise l'adresse IP de la DSi", out_len);

    if (strncmp(r.path, "/api/", 5) != 0)
    {
        if (strcmp(r.method, "GET") != 0)
            return text_reply(405, "Method Not Allowed", "", out_len);
        return serve_static(r.path, out_len);
    }

    if (!cfg.configured)
        return json_error(503, "Service Unavailable",
                          "Choisis d'abord un identifiant sur la DSi", out_len);

    // Protection CSRF : les écritures exigent un en-tête personnalisé
    bool post = strcmp(r.method, "POST") == 0;
    if (post && !r.csrf_ok)
        return json_error(403, "Forbidden", "Requête refusée", out_len);

    if (post && strcmp(r.path, "/api/login") == 0)
        return route_login(&r, out_len);

    if (post && strcmp(r.path, "/api/logout") == 0)
    {
        if (r.cookie_token[0])
            session_delete(r.cookie_token);
        return json_reply(200, "OK", "{\"ok\":true}",
                          "Set-Cookie: dsishop_local=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n",
                          out_len);
    }

    if (!session_valid(r.cookie_token))
        return json_error(401, "Unauthorized", "Non connecté", out_len);

    if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/api/status") == 0)
        return route_status(out_len);
    if (post && strcmp(r.path, "/api/command") == 0)
        return route_command(&r, out_len);

    return json_error(404, "Not Found", "Route inconnue", out_len);
}
