/* tp_plain.c - plaintext TCP transport.
 *
 * One implementation for both hosts: Watt-32 provides a BSD sockets API, so
 * the only divergence from POSIX is stack startup and the non-blocking ioctl.
 */
#include "transport.h"
#include "netinit.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#ifdef DOSYPP_WATT32
#  include <tcp.h>          /* Watt-32; must come before the BSD headers */
#  include <sys/socket.h>
#  include <sys/ioctl.h>    /* FIONBIO */
#  include <sys/werrno.h>   /* EINPROGRESS and friends live here, not errno.h */
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  define CLOSESOCK(fd) close_s(fd)
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define CLOSESOCK(fd) close(fd)
#endif

static char g_lasterr[160];

const char *tp_plain_lasterr(void)
{
    return g_lasterr[0] ? g_lasterr : "no error";
}

static void seterr(const char *what, int use_errno)
{
    if (use_errno)
        snprintf(g_lasterr, sizeof g_lasterr, "%s: %s", what, strerror(errno));
    else
        snprintf(g_lasterr, sizeof g_lasterr, "%s", what);
}

static int set_nonblocking(int fd)
{
#ifdef DOSYPP_WATT32
    char on = 1;
    return ioctlsocket(fd, FIONBIO, &on);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int plain_read(transport *t, void *buf, size_t len)
{
    int n;

    if (t->fd < 0)
        return TP_ERROR;

    n = (int)recv(t->fd, buf, len, 0);
    if (n > 0)
        return n;
    if (n == 0)
        return TP_CLOSED;

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
        return TP_AGAIN;

    seterr("recv", 1);
    return TP_ERROR;
}

static int plain_write(transport *t, const void *buf, size_t len)
{
    int n;

    if (t->fd < 0)
        return TP_ERROR;

    n = (int)send(t->fd, buf, len, 0);
    if (n >= 0)
        return n;

    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
        return TP_AGAIN;

    seterr("send", 1);
    return TP_ERROR;
}

/* Plaintext has no record layer, so nothing is ever buffered above the socket. */
static int plain_pending(transport *t)
{
    (void)t;
    return 0;
}

static void plain_close(transport *t)
{
    if (t->fd >= 0) {
        CLOSESOCK(t->fd);
        t->fd = -1;
    }
}

static void init_vtable(transport *t)
{
    memset(t, 0, sizeof *t);
    t->fd = -1;
    t->t_read = plain_read;
    t->t_write = plain_write;
    t->t_pending = plain_pending;
    t->t_close = plain_close;
}

/* NOT named resolve(): Watt-32's whide.h macro-maps that to _w32_resolve,
   so a static function of that name fails to compile on DOS only. The host
   build cannot catch this class of collision. */
static int resolve_host(const char *host, struct sockaddr_in *sa, int port)
{
    struct hostent *he;

    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);

    /* INADDR_NONE is the documented failure value, but it is not defined
       everywhere; compare at the field's own width. Using (unsigned long)-1
       here is a real bug on a 64-bit host -- the comparison is always false,
       so a bad address silently became 255.255.255.255 instead of being
       looked up by name. It only worked on DOS because long is 32-bit there. */
    sa->sin_addr.s_addr = inet_addr(host);
    if (sa->sin_addr.s_addr == (unsigned int)0xFFFFFFFFu) {
        he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            seterr("cannot resolve hostname", 0);
            return -1;
        }
        memcpy(&sa->sin_addr, he->h_addr_list[0], sizeof sa->sin_addr);
    }
    return 0;
}

int tp_plain_connect_start(transport *t, const char *host, int port)
{
    struct sockaddr_in sa;
    int fd;

    init_vtable(t);

    if (resolve_host(host, &sa, port) != 0)
        return TP_CONN_FAILED;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        seterr("socket", 1);
        return TP_CONN_FAILED;
    }

    /* Non-blocking BEFORE connect, so connect() returns immediately and the
       caller keeps control. A blocking connect to a dead host freezes the
       whole program for the OS timeout -- which on an 80x25 screen with no
       spinner is indistinguishable from a crash. */
    if (set_nonblocking(fd) < 0) {
        seterr("set non-blocking", 1);
        CLOSESOCK(fd);
        return TP_CONN_FAILED;
    }

    t->fd = fd;

#ifndef DOSYPP_WATT32
    {   /* ADC is small-message chatter; Nagle adds latency for nothing. */
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof on);
    }
#endif

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0)
        return TP_CONN_DONE;             /* usual case for loopback */

    if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY)
        return TP_CONN_PENDING;

    seterr("connect", 1);
    plain_close(t);
    return TP_CONN_FAILED;
}

int tp_plain_connect_poll(transport *t)
{
    fd_set w;
    struct timeval tv;
    int err = 0;
    socklen_t len = sizeof err;
    int n;

    if (t->fd < 0)
        return TP_CONN_FAILED;

    FD_ZERO(&w);
    FD_SET(t->fd, &w);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    n = select(t->fd + 1, NULL, &w, NULL, &tv);
    if (n < 0) {
        if (errno == EINTR)
            return TP_CONN_PENDING;
        seterr("select", 1);
        plain_close(t);
        return TP_CONN_FAILED;
    }
    if (n == 0)
        return TP_CONN_PENDING;

    /* Writable means the attempt finished -- successfully or not. SO_ERROR
       is the only way to tell which; a writable socket is NOT proof of a
       successful connect. */
    if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len) < 0) {
        seterr("getsockopt(SO_ERROR)", 1);
        plain_close(t);
        return TP_CONN_FAILED;
    }

    /* Watt-32 reports EISCONN here rather than 0 once the connect has
       completed -- "already connected" is success, not failure. Treating it
       as an error made every DOS connection fail while the host build was
       perfectly happy, which is the whole reason the live rigs exist. */
    if (err != 0 && err != EISCONN) {
        errno = err;
        seterr("connect", 1);
        plain_close(t);
        return TP_CONN_FAILED;
    }

    return TP_CONN_DONE;
}

int tp_plain_connect(transport *t, const char *host, int port,
                     int timeout_secs)
{
    time_t deadline;
    int rc = tp_plain_connect_start(t, host, port);

    if (rc == TP_CONN_DONE)
        return 0;
    if (rc == TP_CONN_FAILED)
        return -1;

    deadline = time(NULL) + (timeout_secs > 0 ? timeout_secs : 30);

    while (time(NULL) <= deadline) {
        rc = tp_plain_connect_poll(t);
        if (rc == TP_CONN_DONE)
            return 0;
        if (rc == TP_CONN_FAILED)
            return -1;
        net_yield();
    }

    seterr("connect timed out", 0);
    plain_close(t);
    return -1;
}

int tp_listen(tp_listener *l, int port)
{
    struct sockaddr_in sa;
    int fd;

    l->fd = -1;
    l->port = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        seterr("socket", 1);
        return -1;
    }

#ifndef DOSYPP_WATT32
    {   /* Without this a restart fails for a minute on the same port,
           which during development is most of the time. */
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof on);
    }
#endif

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        seterr("bind", 1);
        CLOSESOCK(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        seterr("listen", 1);
        CLOSESOCK(fd);
        return -1;
    }

    if (set_nonblocking(fd) < 0) {
        seterr("set non-blocking", 1);
        CLOSESOCK(fd);
        return -1;
    }

    /* Report the port actually bound: with port 0 the system picks one, and
       that number is what has to go into CTM. */
    {
        struct sockaddr_in got;
        socklen_t len = sizeof got;

        if (getsockname(fd, (struct sockaddr *)&got, &len) == 0)
            l->port = (int)ntohs(got.sin_port);
        else
            l->port = port;
    }

    l->fd = fd;
    return 0;
}

void tp_listen_close(tp_listener *l)
{
    if (l->fd >= 0) {
        CLOSESOCK(l->fd);
        l->fd = -1;
    }
}

int tp_listen_port(const tp_listener *l)
{
    return l ? l->port : 0;
}

int tp_accept(tp_listener *l, transport *t)
{
    int fd;

    if (!l || l->fd < 0)
        return -1;

    fd = accept(l->fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
            return 0;
        seterr("accept", 1);
        return -1;
    }

    init_vtable(t);

    if (set_nonblocking(fd) < 0) {
        seterr("set non-blocking", 1);
        CLOSESOCK(fd);
        return -1;
    }

#ifndef DOSYPP_WATT32
    {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof on);
    }
#endif

    t->fd = fd;
    return 1;
}

int tp_write_all(transport *t, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    int spins = 0;

    while (done < len) {
        int n = tp_write(t, p + done, len - done);
        if (n == TP_AGAIN) {
            if (++spins > 100000) {
                seterr("write stalled", 0);
                return -1;
            }
            net_yield();
            continue;
        }
        if (n < 0)
            return -1;
        spins = 0;
        done += (size_t)n;
    }
    return 0;
}
