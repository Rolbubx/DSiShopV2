// Remote local : serveur TCP non bloquant (sockets BSD : lwIP sur DS, POSIX sur PC).
// Appelé à chaque frame depuis la boucle principale : ne bloque jamais.

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "lremote.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_CONNS   4
#define CONN_BUF    (8 * 1024 + 64)
#define CONN_TIMEOUT_S 15

typedef struct {
    int fd;              // -1 : libre
    char *in;
    size_t in_len;
    char *out;
    size_t out_len, out_off;
    time_t started;
} conn_t;

static int listen_fd = -1;
static conn_t conns[MAX_CONNS];

static void set_nonblocking(int fd)
{
    int on = 1;
    ioctl(fd, FIONBIO, (char *)&on);
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

bool lremote_net_start(int port)
{
    if (listen_fd >= 0)
        return true;

    for (int i = 0; i < MAX_CONNS; i++)
    {
        memset(&conns[i], 0, sizeof(conns[i]));
        conns[i].fd = -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0)
    {
        close(fd);
        return false;
    }
    set_nonblocking(fd);
    listen_fd = fd;
    return true;
}

void lremote_net_stop(void)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd >= 0)
            conn_close(&conns[i]);
    if (listen_fd >= 0)
        close(listen_fd);
    listen_fd = -1;
}

bool lremote_net_running(void)
{
    return listen_fd >= 0;
}

static void accept_new(void)
{
    while (1)
    {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            return; // Plus personne en attente (EWOULDBLOCK) ou erreur

        conn_t *c = NULL;
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd < 0)
                c = &conns[i];
        if (!c)
        {
            close(fd); // Trop de connexions simultanées
            continue;
        }
        set_nonblocking(fd);
        c->fd = fd;
        c->in = malloc(CONN_BUF);
        c->in_len = 0;
        c->out = NULL;
        c->started = time(NULL);
        if (!c->in)
            conn_close(c);
    }
}

static void service(conn_t *c)
{
    if (time(NULL) - c->started > CONN_TIMEOUT_S)
    {
        conn_close(c);
        return;
    }

    // 1. Lecture de la requête
    if (!c->out)
    {
        ssize_t n = recv(c->fd, c->in + c->in_len, CONN_BUF - 1 - c->in_len, 0);
        if (n == 0)
        {
            conn_close(c);
            return;
        }
        if (n < 0)
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                conn_close(c);
            return;
        }
        c->in_len += (size_t)n;
        c->in[c->in_len] = '\0';

        long need = lremote_request_length(c->in, c->in_len);
        if (need < 0)
        {
            static const char bad[] = "HTTP/1.1 413 Payload Too Large\r\n"
                                      "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send(c->fd, bad, sizeof(bad) - 1, MSG_NOSIGNAL);
            conn_close(c);
            return;
        }
        if (need == 0)
            return; // Il manque des données

        c->out = lremote_handle(c->in, (size_t)need, &c->out_len);
        c->out_off = 0;
        if (!c->out)
        {
            conn_close(c);
            return;
        }
    }

    // 2. Envoi de la réponse (éventuellement en plusieurs fois)
    while (c->out_off < c->out_len)
    {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                conn_close(c);
            return;
        }
        c->out_off += (size_t)n;
    }

    // Tout est envoyé : "Connection: close"
    shutdown(c->fd, SHUT_WR);
    conn_close(c);
}

void lremote_net_poll(void)
{
    if (listen_fd < 0)
        return;
    accept_new();
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd >= 0)
            service(&conns[i]);
}
