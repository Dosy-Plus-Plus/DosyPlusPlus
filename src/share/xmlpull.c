/* xmlpull.c - the file-list parser. See xmlpull.h for the contract. */
#include "xmlpull.h"

#include <stdio.h>
#include <string.h>

#include "../hash/base32.h"

static void fail(xmlpull *x, const char *why)
{
    if (!x->err[0])
        snprintf(x->err, sizeof x->err, "%s", why);
    x->done = 1;
}

void xp_init(xmlpull *x, xp_fill fill, void *user)
{
    memset(x, 0, sizeof *x);
    x->fill = fill;
    x->user = user;
}

const char *xp_error(const xmlpull *x)
{
    return x->err[0] ? x->err : "no error";
}

const char *xp_attr(const xmlpull *x, const char *key)
{
    int i;

    if (!key)
        return NULL;

    for (i = 0; i < x->nattr; i++)
        if (strcmp(x->attr[i].key, key) == 0)
            return x->attr[i].val;
    return NULL;
}

/* ----------------------------------------------------------- the stream */

/* The next byte, or -1 at the end of input. -2 means a read error. */
static int nextc(xmlpull *x)
{
    if (x->pos < x->len)
        return (unsigned char)x->buf[x->pos++];

    if (x->eof)
        return -1;

    {
        int n = x->fill(x->user, x->buf, sizeof x->buf);

        if (n < 0) {
            x->eof = 1;
            return -2;
        }
        if (n == 0) {
            x->eof = 1;
            return -1;
        }
        x->len = (size_t)n;
        x->pos = 0;
        return (unsigned char)x->buf[x->pos++];
    }
}

static void ungetc_x(xmlpull *x)
{
    if (x->pos > 0)
        x->pos--;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* ------------------------------------------------------------ unescaping */

void xp_unescape(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r != '&') {
            *w++ = *r++;
            continue;
        }

        if (strncmp(r, "&amp;", 5) == 0)       { *w++ = '&';  r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0)   { *w++ = '<';  r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0)   { *w++ = '>';  r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; }
        else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
        else if (r[1] == '#') {
            /* Numeric: &#1234; or &#x4D2;. Encoded back to UTF-8, because
               that is what the rest of the name already is. Anything beyond
               the basic multilingual plane is not worth the code -- a name
               that needs it keeps the escape verbatim and stays legible. */
            unsigned long v = 0;
            char *p = r + 2;
            int hex = (*p == 'x' || *p == 'X');

            if (hex)
                p++;

            if (!*p || *p == ';') {
                *w++ = *r++;           /* "&#;" -- keep it, move on */
                continue;
            }

            while (*p && *p != ';') {
                int d;

                if (*p >= '0' && *p <= '9')      d = *p - '0';
                else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                else break;

                v = v * (hex ? 16u : 10u) + (unsigned long)d;
                if (v > 0xFFFFu)
                    break;             /* absurd; fall through to verbatim */
                p++;
            }

            if (*p != ';' || v == 0 || v > 0xFFFFu) {
                *w++ = *r++;
                continue;
            }

            if (v < 0x80) {
                *w++ = (char)v;
            } else if (v < 0x800) {
                *w++ = (char)(0xC0 | (v >> 6));
                *w++ = (char)(0x80 | (v & 0x3F));
            } else {
                *w++ = (char)(0xE0 | (v >> 12));
                *w++ = (char)(0x80 | ((v >> 6) & 0x3F));
                *w++ = (char)(0x80 | (v & 0x3F));
            }
            r = p + 1;
        } else {
            /* An entity we do not know. Kept verbatim: a mangled name is
               better than a dropped one. */
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* ------------------------------------------------------------- scanning */

/* Read an element or attribute name into dst. Stops on anything that
   cannot be part of a name. */
static int read_name(xmlpull *x, char *dst, size_t dstsz)
{
    size_t n = 0;
    int c;

    for (;;) {
        c = nextc(x);
        if (c == -2) {
            fail(x, "read error in the file list");
            return -1;
        }
        if (c < 0)
            break;

        if (is_space(c) || c == '>' || c == '/' || c == '=' || c == '?') {
            ungetc_x(x);
            break;
        }

        if (n + 1 < dstsz)
            dst[n++] = (char)c;
        /* Over-long names are truncated rather than refused: it is a name,
           not a protocol element. */
    }

    dst[n] = '\0';
    return (int)n;
}

/* Read a quoted attribute value into dst, unescaped. */
static int read_value(xmlpull *x, char *dst, size_t dstsz)
{
    size_t n = 0;
    int quote, c;

    do {
        c = nextc(x);
        if (c == -2) { fail(x, "read error in the file list"); return -1; }
        if (c < 0)   { fail(x, "the file list ends inside a tag"); return -1; }
    } while (is_space(c));

    if (c != '"' && c != '\'') {
        fail(x, "an attribute value is not quoted");
        return -1;
    }
    quote = c;

    for (;;) {
        c = nextc(x);
        if (c == -2) { fail(x, "read error in the file list"); return -1; }
        if (c < 0)   { fail(x, "the file list ends inside an attribute"); return -1; }
        if (c == quote)
            break;
        if (n + 1 < dstsz)
            dst[n++] = (char)c;
    }

    dst[n] = '\0';
    xp_unescape(dst);
    return (int)n;
}

/* Skip a comment, declaration or processing instruction after '<'. The
   leading '!' or '?' has already been consumed. */
static int skip_decl(xmlpull *x, int first)
{
    int c;

    if (first == '!') {
        /* Could be a comment. Comments may contain '>' so they need their
           terminator matched properly. */
        int c1 = nextc(x), c2;

        if (c1 == '-') {
            c2 = nextc(x);
            if (c2 == '-') {
                int dashes = 0;

                for (;;) {
                    c = nextc(x);
                    if (c == -2) { fail(x, "read error in the file list"); return -1; }
                    if (c < 0)   { fail(x, "unterminated comment"); return -1; }
                    if (c == '-') {
                        dashes++;
                    } else if (c == '>' && dashes >= 2) {
                        return 0;
                    } else {
                        dashes = 0;
                    }
                }
            }
            if (c2 < 0) { fail(x, "unterminated declaration"); return -1; }
        } else if (c1 < 0) {
            fail(x, "unterminated declaration");
            return -1;
        }
    }

    /* Everything else: to the next '>'. */
    for (;;) {
        c = nextc(x);
        if (c == -2) { fail(x, "read error in the file list"); return -1; }
        if (c < 0)   { fail(x, "unterminated declaration"); return -1; }
        if (c == '>')
            return 0;
    }
}

/* ---------------------------------------------------------------- events */

xp_event xp_next(xmlpull *x)
{
    char attr[32];

    if (x->done)
        return x->err[0] ? XP_ERROR : XP_END;

    if (x->pending_close) {
        x->pending_close = 0;
        return XP_DIR_CLOSE;
    }

    for (;;) {
        int c;
        int closing = 0;
        int selfclose = 0;
        int is_dir, is_file;
        char elem[48];

        /* To the next tag. Text content is discarded. */
        do {
            c = nextc(x);
            if (c == -2) {
                fail(x, "read error in the file list");
                return XP_ERROR;
            }
            if (c < 0) {
                /* End of input. Unclosed directories are a truncated list,
                   which must not look like a complete one. */
                x->done = 1;
                if (x->depth > 0) {
                    fail(x, "the file list is truncated");
                    return XP_ERROR;
                }
                return XP_END;
            }
        } while (c != '<');

        c = nextc(x);
        if (c == -2) { fail(x, "read error in the file list"); return XP_ERROR; }
        if (c < 0)   { fail(x, "the file list ends after '<'"); return XP_ERROR; }

        if (c == '!' || c == '?') {
            if (skip_decl(x, c) != 0)
                return XP_ERROR;
            continue;
        }

        if (c == '/') {
            closing = 1;
        } else {
            ungetc_x(x);
        }

        if (read_name(x, elem, sizeof elem) < 0)
            return XP_ERROR;
        if (elem[0] == '\0') {
            fail(x, "a tag has no name");
            return XP_ERROR;
        }

        is_dir = (strcmp(elem, "Directory") == 0);
        is_file = (strcmp(elem, "File") == 0);

        if (closing) {
            /* Attributes are not legal on a closing tag; skip to '>'
               anyway rather than refusing a list over whitespace. */
            for (;;) {
                c = nextc(x);
                if (c == -2) { fail(x, "read error in the file list"); return XP_ERROR; }
                if (c < 0)   { fail(x, "unterminated closing tag"); return XP_ERROR; }
                if (c == '>')
                    break;
            }

            if (x->depth == 0) {
                /* More closing tags than opening ones: the tree is not a
                   tree, and continuing would put files in the wrong
                   directories. */
                fail(x, "the file list has unbalanced tags");
                return XP_ERROR;
            }
            x->depth--;

            if (is_dir)
                return XP_DIR_CLOSE;
            continue;               /* </FileListing> and unknowns */
        }

        /* Attributes. */
        x->name[0] = '\0';
        x->size = 0;
        x->tth[0] = '\0';
        x->have_tth = 0;
        x->incomplete = 0;
        x->nattr = 0;
        snprintf(x->elem, sizeof x->elem, "%s", elem);

        for (;;) {
            do {
                c = nextc(x);
                if (c == -2) { fail(x, "read error in the file list"); return XP_ERROR; }
                if (c < 0)   { fail(x, "the file list ends inside a tag"); return XP_ERROR; }
            } while (is_space(c));

            if (c == '/') {
                selfclose = 1;
                continue;           /* expect '>' next */
            }
            if (c == '>')
                break;

            ungetc_x(x);
            if (read_name(x, attr, sizeof attr) < 0)
                return XP_ERROR;
            if (attr[0] == '\0') {
                fail(x, "a tag contains something that is not an attribute");
                return XP_ERROR;
            }

            do {
                c = nextc(x);
                if (c == -2) { fail(x, "read error in the file list"); return XP_ERROR; }
                if (c < 0)   { fail(x, "the file list ends inside a tag"); return XP_ERROR; }
            } while (is_space(c));

            if (c != '=') {
                fail(x, "an attribute has no value");
                return XP_ERROR;
            }

            if (strcmp(attr, "Name") == 0) {
                if (read_value(x, x->name, sizeof x->name) < 0)
                    return XP_ERROR;
                if (x->want_other && x->nattr < XP_MAX_ATTRS) {
                    snprintf(x->attr[x->nattr].key,
                             sizeof x->attr[x->nattr].key, "%s", "Name");
                    snprintf(x->attr[x->nattr].val,
                             sizeof x->attr[x->nattr].val, "%s", x->name);
                    x->nattr++;
                }
            } else if (strcmp(attr, "Size") == 0) {
                char v[32];
                const char *q;

                if (read_value(x, v, sizeof v) < 0)
                    return XP_ERROR;
                x->size = 0;
                for (q = v; *q >= '0' && *q <= '9'; q++)
                    x->size = x->size * 10 + (tth_u64)(*q - '0');
            } else if (strcmp(attr, "Incomplete") == 0) {
                char v[8];

                if (read_value(x, v, sizeof v) < 0)
                    return XP_ERROR;
                /* Anything other than "0" counts: the attribute exists to
                   say "there is more here", and a client writing "true"
                   means the same as one writing "1". */
                x->incomplete = (v[0] != '\0' && strcmp(v, "0") != 0);
            } else if (strcmp(attr, "Base") == 0) {
                /* On <FileListing>: which directory this list is of. Read
                   here rather than through the generic attribute table
                   because that is only filled for callers that asked for
                   unknown elements, and every caller needs this one. */
                if (read_value(x, x->base, sizeof x->base) < 0)
                    return XP_ERROR;
            } else if (strcmp(attr, "TTH") == 0) {
                char v[64];
                unsigned char raw[TTH_HASH_LEN + 4];

                if (read_value(x, v, sizeof v) < 0)
                    return XP_ERROR;

                /* Checked, not trusted: a bad hash here would be sent
                   straight back out in a CGET. */
                if (base32_decode(raw, sizeof raw, v) == TTH_HASH_LEN) {
                    snprintf(x->tth, sizeof x->tth, "%s", v);
                    x->have_tth = 1;
                }
            } else {
                char value[XP_ATTR_VAL];

                if (read_value(x, value, sizeof value) < 0)
                    return XP_ERROR;

                /* Kept for the caller, if it wants elements we have no
                   opinion about. Over the cap they are dropped rather
                   than refused: a list is not worth failing over an
                   attribute nobody asked for. */
                if (x->want_other && x->nattr < XP_MAX_ATTRS) {
                    snprintf(x->attr[x->nattr].key,
                             sizeof x->attr[x->nattr].key, "%s", attr);
                    snprintf(x->attr[x->nattr].val,
                             sizeof x->attr[x->nattr].val, "%s", value);
                    x->nattr++;
                }
            }
        }

        if (is_file) {
            /* A self-closing <File/> is the normal form. One written as
               <File ...></File> is legal too, so it is counted. */
            if (!selfclose)
                x->depth++;

            if (x->name[0] == '\0' || !x->have_tth)
                continue;           /* unusable: no name, or no hash */
            return XP_FILE;
        }

        if (is_dir) {
            /* An empty directory written as <Directory Name="x"/> is still
               a directory: it is reported as open-then-close so a caller
               building a tree does not have to special-case it. */
            if (selfclose) {
                x->pending_close = 1;
                if (x->name[0] == '\0')
                    snprintf(x->name, sizeof x->name, "%s", "(unnamed)");
                return XP_DIR_OPEN;
            }
            if (x->depth >= XP_DEPTH_MAX) {
                fail(x, "the file list is nested absurdly deep");
                return XP_ERROR;
            }
            x->depth++;
            if (x->name[0] == '\0')
                snprintf(x->name, sizeof x->name, "%s", "(unnamed)");
            return XP_DIR_OPEN;
        }

        /* An element we do not care about -- <FileListing> itself, or
           something a newer client added. Counted so the depth stays
           right. */
        if (!selfclose) {
            if (x->depth >= XP_DEPTH_MAX) {
                fail(x, "the file list is nested absurdly deep");
                return XP_ERROR;
            }
            x->depth++;
        }

        /* ...unless the caller asked for them. A hublist is nothing but
           elements this parser has no opinion about. */
        if (x->want_other)
            return XP_OTHER;
    }
}
