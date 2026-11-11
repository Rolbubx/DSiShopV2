// Protocole texte DSi <-> serveur Remote (voir remote_proto.h)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote_proto.h"

#define MAX_FIELDS 5

static void copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

static rcmd_type_t cmd_type(const char *s)
{
    if (strcmp(s, "install") == 0)   return RCMD_INSTALL;
    if (strcmp(s, "uninstall") == 0) return RCMD_UNINSTALL;
    if (strcmp(s, "cancel") == 0)    return RCMD_CANCEL;
    if (strcmp(s, "reboot") == 0)    return RCMD_REBOOT;
    if (strcmp(s, "refresh") == 0)   return RCMD_REFRESH;
    return RCMD_UNKNOWN;
}

void remote_parse_reply(const char *text, rreply_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *p = text;
    while (p && *p)
    {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        char line[512];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        p = end ? end + 1 : NULL;

        // Découpe en champs
        char *f[MAX_FIELDS] = { 0 };
        int n = 0;
        char *s = line;
        while (n < MAX_FIELDS)
        {
            f[n++] = s;
            char *tab = strchr(s, '\t');
            if (!tab)
                break;
            *tab = '\0';
            s = tab + 1;
        }
        for (int i = n; i < MAX_FIELDS; i++)
            f[i] = "";

        if (strcmp(f[0], "owner") == 0)
        {
            out->owner_known = true;
            out->has_owner = strcmp(f[1], "-") != 0 && f[1][0] != '\0';
            if (out->has_owner)
            {
                copy(out->owner_name, sizeof(out->owner_name), f[1]);
                copy(out->owner_email, sizeof(out->owner_email), f[2]);
            }
        }
        else if (strcmp(f[0], "pair") == 0 && f[1][0] && f[2][0])
        {
            out->has_pair = true;
            copy(out->pair_code, sizeof(out->pair_code), f[1]);
            copy(out->pair_url, sizeof(out->pair_url), f[2]);
            out->pair_ttl = atoi(f[3]);
        }
        else if (strcmp(f[0], "device") == 0 && f[1][0] && f[2][0])
        {
            out->has_device = true;
            copy(out->device_id, sizeof(out->device_id), f[1]);
            copy(out->device_token, sizeof(out->device_token), f[2]);
        }
        else if (strcmp(f[0], "cmd") == 0 && f[1][0] && out->ncmds < RREPLY_MAX_CMDS)
        {
            rcmd_t *c = &out->cmds[out->ncmds++];
            copy(c->id, sizeof(c->id), f[1]);
            c->type = cmd_type(f[2]);
            copy(c->arg, sizeof(c->arg), f[3]);
        }
        else if (strcmp(f[0], "error") == 0)
        {
            copy(out->error, sizeof(out->error), f[1]);
        }
    }
}

typedef struct {
    char *buf;
    size_t cap, len;
} sb_t;

// Ajoute un champ en remplaçant tabulations et retours à la ligne
static void sb_field(sb_t *sb, const char *s)
{
    for (; s && *s; s++)
    {
        if (sb->len + 1 >= sb->cap)
            return;
        char c = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
        sb->buf[sb->len++] = c;
    }
    sb->buf[sb->len] = '\0';
}

static void sb_raw(sb_t *sb, const char *fmt, ...)
{
    if (sb->len >= sb->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        sb->len += (size_t)n < sb->cap - sb->len ? (size_t)n : sb->cap - sb->len - 1;
}

size_t remote_build_status(char *buf, size_t cap, const rstatus_t *st,
                           const rack_t *acks, int nacks,
                           const game_t *games, int ngames)
{
    sb_t sb = { buf, cap, 0 };
    if (cap == 0)
        return 0;
    buf[0] = '\0';

    sb_raw(&sb, "state\t");
    sb_field(&sb, st->state);
    sb_raw(&sb, "\nmessage\t");
    sb_field(&sb, st->message);
    sb_raw(&sb, "\nprogress\t%llu\t%llu\n", (unsigned long long)st->now,
           (unsigned long long)st->total);
    if (st->file && st->file[0])
    {
        sb_raw(&sb, "file\t");
        sb_field(&sb, st->file);
        sb_raw(&sb, "\n");
    }
    if (st->release && st->release[0])
    {
        sb_raw(&sb, "release\t");
        sb_field(&sb, st->release);
        sb_raw(&sb, "\n");
    }
    if (st->version)
    {
        sb_raw(&sb, "version\t");
        sb_field(&sb, st->version);
        sb_raw(&sb, "\n");
    }
    if (st->free_bytes)
        sb_raw(&sb, "free\t%llu\n", (unsigned long long)st->free_bytes);

    for (int i = 0; i < nacks; i++)
    {
        sb_raw(&sb, "ack\t");
        sb_field(&sb, acks[i].id);
        sb_raw(&sb, "\t%s\t", acks[i].ok ? "ok" : "error");
        sb_field(&sb, acks[i].message);
        sb_raw(&sb, "\n");
    }

    for (int i = 0; i < ngames; i++)
    {
        sb_raw(&sb, "game\t");
        sb_field(&sb, games[i].file);
        sb_raw(&sb, "\t%llu\t", (unsigned long long)games[i].size);
        sb_field(&sb, games[i].source);
        sb_raw(&sb, "\t");
        sb_field(&sb, games[i].version);
        sb_raw(&sb, "\n");
    }

    return sb.len;
}
