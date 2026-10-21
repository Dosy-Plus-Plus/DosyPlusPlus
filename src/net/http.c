/* http.c - just enough HTTP to fetch a hub list. See http.h. */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "netinit.h"

#define HTTP_LINE_MAX 512
#define HTTP_CHUNK    4096

static char g_err[200];

const char *http_error(void)
{
    return g_err[0] ? g_err : "no error";
}

static void seterr(const char *fmt, const char *a)
{
    if (a)
        snprintf(g_err, sizeof g_err, fmt, a);
    else
        snprintf(g_err, sizeof g_err, "%s", fmt);
}

/* ------------------------------------------------------------------ URL */

int http_split_url(const char *url, char *host_out, size_t host_sz,
                   int *port_out, char *path_out, size_t path_sz)
{
    const char *p = url;
    const char *slash;
    const char *colon;
    size_t hostlen;

    if (!url || !host_out || !path_out || host_sz == 0 || path_sz == 0)
        return -1;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        if (port_out)
            *port_out = 80;
    } else if (strncmp(p, "https://", 8) == 0) {
        /* Said plainly rather than attempted: there is no TLS in this
           binary, and a confident-looking failure further down would be
           worse than this. */
        seterr("https is not supported -- no TLS in DOSy++ yet; use an "
               "http:// list", NULL);
        return -1;
    } else if (strstr(p, "://") != NULL) {
        seterr("only http:// URLs can be fetched", NULL);
        return -1;
    } else {
        /* No scheme: treat it as a host, which is what someone typing an
           address into a settings field will do. */
        if (port_out)
            *port_out = 80;
    }

    slash = strchr(p, '/');
    hostlen = slash ? (size_t)(slash - p) : strlen(p);

    /* A port on the host part. Searched within the host only, so a colon
       later in the path is not mistaken for one. */
    colon = memchr(p, ':', hostlen);
    if (colon) {
        int port = 0;
        const char *d = colon + 1;

        while (d < p + hostlen && *d >= '0' && *d <= '9')
            port = port * 10 + (*d++ - '0');

        if (port <= 0 || port > 65535) {
            seterr("the URL has an unusable port", NULL);
            return -1;
        }
        if (port_out)
            *port_out = port;
        hostlen = (size_t)(colon - p);
    }

    if (hostlen == 0) {
        seterr("the URL has no host", NULL);
        return -1;
    }
    if (hostlen >= host_sz) {
        seterr("the host in that URL is too long", NULL);
        return -1;
    }

    memcpy(host_out, p, hostlen);
    host_out[hostlen] = '\0';

    if (slash)
        snprintf(path_out, path_sz, "%s", slash);
    else
        snprintf(path_out, path_sz, "%s", "/");

    return 0;
}

/* --------------------------------------------------------------- reading */

/* One line, up to and including CRLF, which is discarded. Returns the
   length, or -1 on error/EOF before a line was complete. */
static int read_line(transport *t, char *out, size_t outsz,
                     char *carry, size_t *carry_len)
{
    size_t n = 0;

    for (;;) {
        size_t i;

        /* Anything already buffered. */
        for (i = 0; i < *carry_len; i++) {
            char c = carry[i];

            if (c == '\n') {
                size_t rest = *carry_len - i - 1;

                memmove(carry, carry + i + 1, rest);
                *carry_len = rest;

                while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n'))
                    n--;
                out[n] = '\0';
                return (int)n;
            }
            if (n + 1 < outsz)
                out[n++] = c;
        }
        *carry_len = 0;

        {
            int got = tp_read(t, carry, HTTP_LINE_MAX);

            if (got == TP_AGAIN) {
                net_yield();
                continue;
            }
            if (got <= 0)
                return -1;
            *carry_len = (size_t)got;
        }
    }
}

/* ---------------------------------------------------------------- fetch */

static long fetch_once(const char *url, const char *path,
                       http_progress progress, void *user,
                       char *redirect, size_t redirect_sz)
{
    char host[128];
    char req_path[256];
    char line[HTTP_LINE_MAX];
    char carry[HTTP_LINE_MAX];
    size_t carry_len = 0;
    transport t;
    FILE *out = NULL;
    int port = 80;
    int status = 0;
    unsigned long total = 0;
    unsigned long got = 0;

    redirect[0] = '\0';

    if (http_split_url(url, host, sizeof host, &port, req_path,
                       sizeof req_path) != 0)
        return -1;

    if (tp_plain_connect(&t, host, port, 20) != 0) {
        snprintf(g_err, sizeof g_err, "cannot reach %s:%d: %s",
                 host, port, tp_plain_lasterr());
        return -1;
    }

    /* HTTP/1.0 with an explicit close: no keep-alive to manage and no
       chunked encoding to decode. Identity encoding only -- a hublist is
       already compressed and we have no gzip. */
    snprintf(line, sizeof line,
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: DOSy++/0.1 (MS-DOS)\r\n"
             "Accept: */*\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n"
             "\r\n",
             req_path, host);

    if (tp_write_all(&t, line, strlen(line)) != 0) {
        seterr("the connection closed before the request went out", NULL);
        tp_close(&t);
        return -1;
    }

    /* Status line. */
    if (read_line(&t, line, sizeof line, carry, &carry_len) < 0) {
        seterr("no answer from the server", NULL);
        tp_close(&t);
        return -1;
    }
    if (strncmp(line, "HTTP/", 5) != 0) {
        seterr("that is not an HTTP server", NULL);
        tp_close(&t);
        return -1;
    }
    {
        const char *sp = strchr(line, ' ');

        if (sp)
            status = (int)strtol(sp + 1, NULL, 10);
    }

    /* Headers. Only two matter. */
    for (;;) {
        int n = read_line(&t, line, sizeof line, carry, &carry_len);

        if (n < 0) {
            seterr("the headers ended early", NULL);
            tp_close(&t);
            return -1;
        }
        if (n == 0)
            break;                      /* blank line: body follows */

        if (strncmp(line, "Content-Length:", 15) == 0)
            total = strtoul(line + 15, NULL, 10);
        else if (strncmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;

            while (*v == ' ')
                v++;
            snprintf(redirect, redirect_sz, "%s", v);
        }
    }

    if (status >= 300 && status < 400 && redirect[0]) {
        tp_close(&t);
        return -2;                      /* the caller follows it */
    }

    if (status != 200) {
        snprintf(g_err, sizeof g_err, "the server said %d", status);
        tp_close(&t);
        return -1;
    }

    if (total > HTTP_MAX_BODY) {
        snprintf(g_err, sizeof g_err,
                 "that file is %lu bytes, which is more than this machine "
                 "will take", total);
        tp_close(&t);
        return -1;
    }

    out = fopen(path, "wb");
    if (!out) {
        seterr("cannot write %s", path);
        tp_close(&t);
        return -1;
    }

    /* Whatever the header reader left over is the start of the body. */
    if (carry_len > 0) {
        if (fwrite(carry, 1, carry_len, out) != carry_len) {
            seterr("disk full or write error", NULL);
            fclose(out);
            remove(path);
            tp_close(&t);
            return -1;
        }
        got = (unsigned long)carry_len;
    }

    for (;;) {
        char buf[HTTP_CHUNK];
        int n = tp_read(&t, buf, sizeof buf);

        if (n == TP_AGAIN) {
            net_yield();
            if (progress && progress(user, got, total)) {
                fclose(out);
                remove(path);
                tp_close(&t);
                seterr("cancelled", NULL);
                return -1;
            }
            continue;
        }
        if (n == TP_CLOSED)
            break;                      /* HTTP/1.0: close means done */
        if (n < 0) {
            seterr("the connection broke mid-transfer", NULL);
            fclose(out);
            remove(path);
            tp_close(&t);
            return -1;
        }

        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            seterr("disk full or write error", NULL);
            fclose(out);
            remove(path);
            tp_close(&t);
            return -1;
        }
        got += (unsigned long)n;

        if (got > HTTP_MAX_BODY) {
            seterr("the server is sending more than this machine will "
                   "take", NULL);
            fclose(out);
            remove(path);
            tp_close(&t);
            return -1;
        }

        if (progress && progress(user, got, total)) {
            fclose(out);
            remove(path);
            tp_close(&t);
            seterr("cancelled", NULL);
            return -1;
        }
    }

    tp_close(&t);

    if (fclose(out) != 0) {
        seterr("could not finish writing %s", path);
        remove(path);
        return -1;
    }

    /* A truncated transfer must not look like a short file: a half a
       hublist parses into half a hublist and nothing says why. */
    if (total > 0 && got < total) {
        snprintf(g_err, sizeof g_err,
                 "got %lu bytes of %lu -- the transfer was cut short",
                 got, total);
        remove(path);
        return -1;
    }

    return (long)got;
}

long http_get(const char *url, const char *path,
              http_progress progress, void *user)
{
    char current[512];
    char next[512];
    int hop;

    g_err[0] = '\0';
    snprintf(current, sizeof current, "%s", url ? url : "");

    for (hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
        long rc = fetch_once(current, path, progress, user, next,
                             sizeof next);

        if (rc != -2)
            return rc;

        /* Redirected. Only to an absolute http:// URL: resolving a
           relative one properly is more URL handling than this needs, and
           hublist redirects are absolute. */
        if (strncmp(next, "http://", 7) != 0) {
            snprintf(g_err, sizeof g_err,
                     "redirected to \"%s\", which this client will not "
                     "follow", next);
            return -1;
        }
        snprintf(current, sizeof current, "%s", next);
    }

    seterr("too many redirects", NULL);
    return -1;
}
