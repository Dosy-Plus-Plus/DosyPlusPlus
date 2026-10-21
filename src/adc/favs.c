/* favs.c - the favourites list. See favs.h. */
#include "favs.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

static void set_str(char *dst, size_t dstsz, const char *src)
{
    if (!src)
        src = "";
    snprintf(dst, dstsz, "%s", src);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    char *p = s;

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';

    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
}

static int yes(const char *v)
{
    return strcmp(v, "no") != 0 && strcmp(v, "0") != 0 &&
           strcmp(v, "off") != 0 && v[0] != '\0';
}

/* The part of an address that identifies the hub: no scheme, no trailing
   slash, case-folded. Two entries that differ only in those are the same
   hub, and keeping both would mean two passwords for one account. */
static void canon(const char *addr, char *out, size_t outsz)
{
    size_t n;

    snprintf(out, outsz, "%s", addr ? addr : "");

    /* Folded first, then the scheme stripped: a hub list that writes
       "ADC://Hub.Example" is naming the same hub as one that writes it in
       lower case, and keeping both would mean two passwords for one
       account. */
    for (n = 0; out[n]; n++)
        if (out[n] >= 'A' && out[n] <= 'Z')
            out[n] += 32;

    if (strncmp(out, "adc://", 6) == 0)
        memmove(out, out + 6, strlen(out + 6) + 1);
    else if (strncmp(out, "adcs://", 7) == 0)
        memmove(out, out + 7, strlen(out + 7) + 1);

    n = strlen(out);
    while (n > 0 && (out[n - 1] == '/' || out[n - 1] == ' '))
        out[--n] = '\0';
}

void favs_init(favs *f)
{
    memset(f, 0, sizeof *f);
}

int favs_split(const char *addr, char *host_out, size_t host_sz,
               int *port_out)
{
    char work[FAV_ADDR_MAX];
    char *colon;

    if (!addr || !host_out || host_sz == 0)
        return -1;

    canon(addr, work, sizeof work);
    if (work[0] == '\0')
        return -1;

    if (port_out)
        *port_out = 1511;

    /* A path after the host, which public lists sometimes carry. */
    {
        char *slash = strchr(work, '/');

        if (slash)
            *slash = '\0';
    }

    colon = strrchr(work, ':');
    if (colon) {
        int v = 0;
        const char *d = colon + 1;
        int digits = 0;

        /* Five digits is every port there is. Without the cap a long run
           of digits overflows an int on the way to the range check below,
           which is undefined behaviour reached from a config file. */
        while (*d >= '0' && *d <= '9' && digits < 5) {
            v = v * 10 + (*d++ - '0');
            digits++;
        }

        if (v > 0 && v < 65536 && *d == '\0') {
            if (port_out)
                *port_out = v;
            *colon = '\0';
        }
    }

    if (work[0] == '\0')
        return -1;

    snprintf(host_out, host_sz, "%s", work);
    return 0;
}

/* ------------------------------------------------------------- reading */

int favs_load(favs *f, const char *path)
{
    FILE *fp;
    char line[256];
    fav_entry *cur = NULL;

    favs_init(f);

    fp = fopen(path, "r");
    if (!fp)
        return 0;               /* first run: nobody has favourites yet */

    while (fgets(line, sizeof line, fp)) {
        char *eq;

        trim(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
            continue;

        if (line[0] == '[') {
            /* A new entry. Anything in brackets starts one: the name of
               the section is not worth validating when there is only one
               kind of section. */
            if (f->count >= FAVS_MAX) {
                snprintf(f->err, sizeof f->err,
                         "more than %d favourites; the rest were ignored",
                         FAVS_MAX);
                cur = NULL;
                continue;
            }
            cur = &f->list[f->count++];
            memset(cur, 0, sizeof *cur);
            continue;
        }

        eq = strchr(line, '=');
        if (!eq || !cur)
            continue;           /* a stray line: skip it, keep the file */

        *eq = '\0';
        trim(line);
        {
            char *val = eq + 1;

            trim(val);

            if (strcmp(line, "name") == 0)
                set_str(cur->name, sizeof cur->name, val);
            else if (strcmp(line, "address") == 0)
                set_str(cur->addr, sizeof cur->addr, val);
            else if (strcmp(line, "nick") == 0)
                set_str(cur->nick, sizeof cur->nick, val);
            else if (strcmp(line, "password") == 0)
                set_str(cur->pass, sizeof cur->pass, val);
            else if (strcmp(line, "auto") == 0)
                cur->autoconnect = yes(val);
        }
    }

    fclose(fp);

    /* Entries with no address are no use: they came from a half-written
       file or a hand edit that went wrong. Dropped rather than offered as
       a hub that cannot be dialled. */
    {
        int i, w = 0;

        for (i = 0; i < f->count; i++) {
            if (f->list[i].addr[0] == '\0')
                continue;
            if (f->list[i].name[0] == '\0')
                set_str(f->list[i].name, sizeof f->list[i].name,
                        f->list[i].addr);
            if (w != i)
                f->list[w] = f->list[i];
            w++;
        }
        f->count = w;
    }

    /* Only one entry may be automatic. A file claiming two is a hand-edit
       mistake; the first wins rather than the program refusing to start. */
    {
        int i, seen = 0;

        for (i = 0; i < f->count; i++) {
            if (!f->list[i].autoconnect)
                continue;
            if (seen)
                f->list[i].autoconnect = 0;
            seen = 1;
        }
    }

    f->dirty = 0;
    return f->count;
}

int favs_save(const favs *f, const char *path)
{
    FILE *fp = fopen(path, "w");
    int i;

    if (!fp)
        return -1;

    fprintf(fp, "; DOSy++ favourite hubs. Written by the HUBS door.\n");
    fprintf(fp, ";\n");
    fprintf(fp, "; The passwords here are in the clear. There is nowhere\n");
    fprintf(fp, "; better on a DOS machine -- no keychain, no file\n");
    fprintf(fp, "; permissions -- and hiding them would only misrepresent\n");
    fprintf(fp, "; how safe they are. Anyone who can read this disk can\n");
    fprintf(fp, "; read them.\n");
    fprintf(fp, ";\n");
    fprintf(fp, "; nick and password may be left empty: the ones in\n");
    fprintf(fp, "; DOSYPP.INI are used instead.\n");

    for (i = 0; i < f->count; i++) {
        const fav_entry *e = &f->list[i];

        fprintf(fp, "\n[hub]\n");
        fprintf(fp, "name     = %s\n", e->name);
        fprintf(fp, "address  = %s\n", e->addr);
        fprintf(fp, "nick     = %s\n", e->nick);
        fprintf(fp, "password = %s\n", e->pass);
        fprintf(fp, "auto     = %s\n", e->autoconnect ? "yes" : "no");
    }

    if (fclose(fp) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------ changing */

fav_entry *favs_find(favs *f, const char *addr)
{
    char want[FAV_ADDR_MAX];
    char have[FAV_ADDR_MAX];
    int i;

    if (!addr || !addr[0])
        return NULL;

    canon(addr, want, sizeof want);

    for (i = 0; i < f->count; i++) {
        canon(f->list[i].addr, have, sizeof have);
        if (strcmp(want, have) == 0)
            return &f->list[i];
    }
    return NULL;
}

fav_entry *favs_add(favs *f, const char *name, const char *addr)
{
    fav_entry *e;

    if (!addr || !addr[0])
        return NULL;

    /* Already there: update the name and keep everything else. Adding a
       hub twice must not lose the password that was set on it. */
    e = favs_find(f, addr);
    if (e) {
        if (name && name[0])
            set_str(e->name, sizeof e->name, name);
        f->dirty = 1;
        return e;
    }

    if (f->count >= FAVS_MAX) {
        snprintf(f->err, sizeof f->err,
                 "the favourites list is full (%d)", FAVS_MAX);
        return NULL;
    }

    e = &f->list[f->count++];
    memset(e, 0, sizeof *e);
    set_str(e->addr, sizeof e->addr, addr);
    set_str(e->name, sizeof e->name, (name && name[0]) ? name : addr);
    f->dirty = 1;
    return e;
}

int favs_remove(favs *f, int index)
{
    int i;

    if (index < 0 || index >= f->count)
        return 0;

    for (i = index; i + 1 < f->count; i++)
        f->list[i] = f->list[i + 1];

    f->count--;
    f->dirty = 1;
    return 1;
}

fav_entry *favs_autoconnect(favs *f)
{
    int i;

    for (i = 0; i < f->count; i++)
        if (f->list[i].autoconnect)
            return &f->list[i];
    return NULL;
}

void favs_set_autoconnect(favs *f, int index)
{
    int i;
    int was;

    if (index < 0 || index >= f->count)
        return;

    was = f->list[index].autoconnect;

    for (i = 0; i < f->count; i++)
        f->list[i].autoconnect = 0;

    /* Pressing it on the one that already has it turns it off, so there is
       a way back to "do not connect to anything on startup". */
    f->list[index].autoconnect = !was;
    f->dirty = 1;
}
