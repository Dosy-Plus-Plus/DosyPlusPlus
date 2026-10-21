/* tokenize.c - ADC line framing, escaping, and message parsing. */
#include "tokenize.h"

#include <string.h>

/* ---------------------------------------------------------------- escaping */

int adc_unescape(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;

    if (dstsz == 0)
        return -1;

    while (*src) {
        char c = *src++;

        if (c == '\\') {
            char e = *src;
            if (e == 's')      { c = ' ';  src++; }
            else if (e == 'n') { c = '\n'; src++; }
            else if (e == '\\'){ c = '\\'; src++; }
            /* An undefined escape is kept verbatim rather than dropped: a
               hub sending one is buggy, but losing a byte would corrupt a
               filename, and filenames are addressed by hash downstream. */
        }

        if (o + 1 >= dstsz)
            return -1;
        dst[o++] = c;
    }

    dst[o] = '\0';
    return (int)o;
}

int adc_escape(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;

    if (dstsz == 0)
        return -1;

    while (*src) {
        char c = *src++;
        const char *rep = NULL;

        if (c == ' ')       rep = "\\s";
        else if (c == '\n') rep = "\\n";
        else if (c == '\\') rep = "\\\\";

        if (rep) {
            if (o + 2 >= dstsz)
                return -1;
            dst[o++] = rep[0];
            dst[o++] = rep[1];
        } else {
            if (o + 1 >= dstsz)
                return -1;
            dst[o++] = c;
        }
    }

    dst[o] = '\0';
    return (int)o;
}

/* ----------------------------------------------------------------- parsing */

int adc_hdr_params(char type)
{
    switch (type) {
    case 'B': case 'U': return 1;
    case 'D': case 'E': case 'F': return 2;
    default: return 0;
    }
}

int adc_has_sid(char type)
{
    return type == 'B' || type == 'D' || type == 'E' || type == 'F';
}

static int valid_type(char c)
{
    return c == 'H' || c == 'I' || c == 'B' || c == 'C' ||
           c == 'D' || c == 'E' || c == 'F' || c == 'U';
}

static int valid_cmd_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int adc_parse(adc_msg *m, const char *line)
{
    size_t len, i;
    char *p, *end;

    memset(m, 0, sizeof *m);

    if (!line)
        return -1;

    len = strlen(line);

    /* An empty line is a keepalive, not a malformed message. */
    if (len == 0)
        return 0;

    if (len >= sizeof m->store)
        return -1;
    if (len < 4)
        return -1;

    if (!valid_type(line[0]))
        return -1;
    for (i = 1; i < 4; i++)
        if (!valid_cmd_char(line[i]))
            return -1;
    /* The command must be followed by a separator or end of message. */
    if (len > 4 && line[4] != ' ')
        return -1;

    memcpy(m->store, line, len + 1);

    m->type = m->store[0];
    memcpy(m->cmd, m->store + 1, 3);
    m->cmd[3] = '\0';

    if (len == 4)
        return 0;

    /* Split in place on single spaces. Empty tokens (a hub emitting a double
       space) are skipped rather than kept, matching DC++'s tolerance. */
    p = m->store + 5;
    end = m->store + len;

    while (p < end) {
        char *sp = memchr(p, ' ', (size_t)(end - p));
        char *tok = p;

        if (sp) {
            *sp = '\0';
            p = sp + 1;
        } else {
            p = end;
        }

        if (*tok == '\0')
            continue;

        if (m->nparam >= ADC_MAX_PARAMS)
            return -1;

        /* Unescaping only ever shrinks, so it is safe in place. */
        if (adc_unescape(tok, strlen(tok) + 1, tok) < 0)
            return -1;

        m->param[m->nparam++] = tok;
    }

    return 0;
}

static int looks_like_field(const char *s)
{
    return valid_cmd_char(s[0]) && s[1] != '\0' && valid_cmd_char(s[1]);
}

const char *adc_field(const adc_msg *m, const char *key)
{
    int i = adc_hdr_params(m->type);

    for (; i < m->nparam; i++) {
        const char *s = m->param[i];
        if (!looks_like_field(s))
            continue;
        if (s[0] == key[0] && s[1] == key[1])
            return s + 2;
    }
    return NULL;
}

/* ----------------------------------------------------------------- framing */

void linebuf_init(linebuf *lb)
{
    lb->len = 0;
    lb->overflow = 0;
}

int linebuf_feed(linebuf *lb, const char *data, size_t len)
{
    int rc = 0;

    while (len > 0) {
        size_t n;

        if (lb->overflow) {
            /* Discard until the end of the offending line, then resync. */
            const char *nl = memchr(data, '\n', len);
            if (!nl)
                return -1;
            len -= (size_t)(nl - data) + 1;
            data = nl + 1;
            lb->overflow = 0;
            lb->len = 0;
            continue;
        }

        n = sizeof lb->buf - 1 - lb->len;
        if (n > len)
            n = len;

        if (n == 0) {
            /* No newline in a full buffer: the line is over-long. */
            lb->overflow = 1;
            lb->len = 0;
            rc = -1;
            continue;
        }

        memcpy(lb->buf + lb->len, data, n);
        lb->len += n;
        data += n;
        len -= n;
    }

    return rc;
}

size_t linebuf_pending(const linebuf *lb, const char **data)
{
    if (data)
        *data = lb->buf;
    return lb->len;
}

void linebuf_consume(linebuf *lb, size_t n)
{
    if (n >= lb->len) {
        lb->len = 0;
        return;
    }
    memmove(lb->buf, lb->buf + n, lb->len - n);
    lb->len -= n;
}

int linebuf_next(linebuf *lb, char *out)
{
    char *nl = memchr(lb->buf, '\n', lb->len);
    size_t linelen, rest;

    if (!nl)
        return 0;

    linelen = (size_t)(nl - lb->buf);
    memcpy(out, lb->buf, linelen);
    out[linelen] = '\0';

    rest = lb->len - linelen - 1;
    memmove(lb->buf, nl + 1, rest);
    lb->len = rest;

    return 1;
}

void adc_u64_dec(tiger_u64 v, char *out, size_t outsz)
{
    char tmp[24];
    int n = 0;

    if (outsz == 0)
        return;

    /* Built backwards: there is no 64-bit printf on this target, which is
       the whole reason this function exists. */
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v > 0 && n < (int)sizeof tmp);

    if ((size_t)n >= outsz)
        n = (int)outsz - 1;

    {
        int i;

        for (i = 0; i < n; i++)
            out[i] = tmp[n - 1 - i];
        out[n] = '\0';
    }
}

tiger_u64 adc_u64_parse(const char *s)
{
    tiger_u64 v = 0;
    const tiger_u64 cap = (tiger_u64)0xFFFFFFFFUL * 0xFFFFFFFFUL;

    if (!s)
        return 0;

    while (*s >= '0' && *s <= '9') {
        tiger_u64 digit = (tiger_u64)(*s - '0');

        /* Saturate. A wrapped size would be acted on: a 2^64+small file
           would read as small, be queued, and fail at the first hash
           check -- or worse, succeed against the wrong bytes. */
        if (v > (cap - digit) / 10)
            return cap;
        v = v * 10 + digit;
        s++;
    }
    return v;
}
