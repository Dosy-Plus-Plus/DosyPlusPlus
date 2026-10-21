/* hublist.c - fetching and parsing a public hub list. See hublist.h. */
#include "hublist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net/http.h"
#include "../share/bz.h"
#include "../share/xmlpull.h"
#include "../hash/tiger.h"

void hublist_init(hublist *hl)
{
    memset(hl, 0, sizeof *hl);
}

/* ------------------------------------------------------------- parsing */

/* The two shapes a list arrives in. The caller's file name is no guide:
   it comes from a URL, and URLs promising .xml.bz2 serve plain XML often
   enough to matter. */
static int looks_like_bzip2(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char magic[3];
    size_t n;

    if (!f)
        return 0;
    n = fread(magic, 1, sizeof magic, f);
    fclose(f);

    return n == 3 && magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h';
}

static int fill_bz(void *user, void *buf, size_t len)
{
    return bz_read((bz_reader *)user, buf, len);
}

static int fill_file(void *user, void *buf, size_t len)
{
    size_t n = fread(buf, 1, len, (FILE *)user);

    if (n == 0 && ferror((FILE *)user))
        return -1;
    return (int)n;
}

/* Saturating, not wrapping, and not stopping part way through a number:
   a hub claiming more users than fit in a long must come out as "a lot"
   rather than as whatever the low digits happened to be. A wrapped count
   would sort a dead hub to the top of the list. */
static unsigned long to_ulong(const char *s)
{
    unsigned long v = 0;
    int saturated = 0;

    if (!s)
        return 0;
    while (*s == ' ')
        s++;

    while (*s >= '0' && *s <= '9') {
        if (!saturated) {
            if (v > (0xFFFFFFFFUL - (unsigned long)(*s - '0')) / 10UL)
                saturated = 1;
            else
                v = v * 10 + (unsigned long)(*s - '0');
        }
        s++;
    }

    return saturated ? 0xFFFFFFFFUL : v;
}

/* Shared bytes, as gigabytes. A busy hub's total passes 32 bits -- ten
   terabytes is ordinary -- so this parses into 64 bits and divides.
   tth_u64 is the project's 64-bit integer; there is no other. */
static unsigned long to_gb(const char *s)
{
    tth_u64 v = 0;

    if (!s)
        return 0;
    while (*s == ' ')
        s++;

    while (*s >= '0' && *s <= '9') {
        /* Saturate rather than wrap, as above. */
        if (v > (tth_u64)0xFFFFFFFFFFFFFFFFULL / 10)
            break;
        v = v * 10 + (tth_u64)(*s - '0');
        s++;
    }

    v /= (tth_u64)(1024 * 1024) * 1024;
    if (v > 0xFFFFFFFFUL)
        v = 0xFFFFFFFFUL;
    return (unsigned long)v;
}

/* Is this an address this client can dial? A list is mostly ADC these
   days but still carries NMDC hubs, and offering one we cannot speak to
   would be a connection that fails for reasons the user cannot see. */
static int usable_address(const char *addr)
{
    if (!addr || !addr[0])
        return 0;
    if (strncmp(addr, "adc://", 6) == 0)
        return 1;
    if (strncmp(addr, "adcs://", 7) == 0)
        return 0;       /* TLS, which this client does not have */
    if (strncmp(addr, "dchub://", 8) == 0)
        return 0;       /* NMDC: a different protocol entirely */
    if (strstr(addr, "://") != NULL)
        return 0;
    /* No scheme at all: lists do this, and it means ADC on the default
       port as far as anything here is concerned. */
    return 1;
}

static int parse_stream(hublist *hl, xp_fill fill, void *user)
{
    xmlpull x;

    xp_init(&x, fill, user);
    x.want_other = 1;           /* a hublist is <Hub .../> and nothing else */

    for (;;) {
        xp_event ev = xp_next(&x);
        const char *addr;
        const char *name;

        if (ev == XP_END)
            break;
        if (ev == XP_ERROR) {
            snprintf(hl->err, sizeof hl->err, "%s", xp_error(&x));
            /* Keep what was parsed: half a hub list is still a list of
               hubs, as long as it is not presented as all of them. */
            hl->truncated = 1;
            break;
        }
        if (ev != XP_OTHER)
            continue;
        if (strcmp(x.elem, "Hub") != 0)
            continue;

        addr = xp_attr(&x, "Address");
        if (!addr)
            addr = xp_attr(&x, "Server");       /* older lists */
        name = xp_attr(&x, "Name");

        if (!addr || !addr[0])
            continue;

        if (!usable_address(addr)) {
            hl->skipped++;
            continue;
        }

        /* Where this entry goes. Once the table is full the quietest
           entry is replaced -- the file is in no useful order and the tail
           of a public list is mostly hubs nobody is on. */
        {
            int slot = hl->count;

            if (slot >= HUBLIST_MAX) {
                int worst = 0;
                int i;

                for (i = 1; i < hl->count; i++)
                    if (hl->entries[i].users < hl->entries[worst].users)
                        worst = i;

                hl->truncated = 1;

                if (to_ulong(xp_attr(&x, "Users")) <=
                    hl->entries[worst].users)
                    continue;       /* quieter than everything we have */

                slot = worst;
            }

            {
            hublist_entry *e = &hl->entries[slot];
            const char *desc = xp_attr(&x, "Description");

            memset(e, 0, sizeof *e);
            snprintf(e->addr, sizeof e->addr, "%s", addr);
            snprintf(e->name, sizeof e->name, "%s",
                     (name && name[0]) ? name : addr);
            if (desc)
                snprintf(e->desc, sizeof e->desc, "%s", desc);
            e->users = to_ulong(xp_attr(&x, "Users"));
            e->shared_gb = to_gb(xp_attr(&x, "Shared"));

            if (slot == hl->count && hl->count < HUBLIST_MAX)
                hl->count++;
            }
        }
    }

    return hl->count;
}

int hublist_load(hublist *hl, const char *path)
{
    int n;

    hl->count = 0;
    hl->truncated = 0;
    hl->skipped = 0;
    hl->err[0] = '\0';

    if (looks_like_bzip2(path)) {
        bz_reader *r = bz_open(path);

        if (!r) {
            snprintf(hl->err, sizeof hl->err, "%s", bz_error());
            return -1;
        }
        n = parse_stream(hl, fill_bz, r);
        bz_close(&r);
    } else {
        FILE *f = fopen(path, "rb");

        if (!f) {
            snprintf(hl->err, sizeof hl->err, "cannot read %s", path);
            return -1;
        }
        n = parse_stream(hl, fill_file, f);
        fclose(f);
    }

    if (n == 0 && hl->err[0])
        return -1;

    hublist_sort(hl);
    return n;
}

int hublist_fetch(hublist *hl, const char *url,
                  int (*progress)(void *user, unsigned long got,
                                  unsigned long total),
                  void *user)
{
    long bytes;

    hublist_init(hl);

    bytes = http_get(url, HUBLIST_FILE, progress, user);
    if (bytes < 0) {
        snprintf(hl->err, sizeof hl->err, "%s", http_error());
        return -1;
    }
    if (bytes == 0) {
        snprintf(hl->err, sizeof hl->err, "the list came back empty");
        remove(HUBLIST_FILE);
        return -1;
    }

    {
        int n = hublist_load(hl, HUBLIST_FILE);

        /* The download is a temporary. Keeping it would mean the next
           fetch could silently show the last one. */
        remove(HUBLIST_FILE);
        return n;
    }
}

void hublist_sort(hublist *hl)
{
    int i, j;

    /* Insertion sort: 400 entries at most, and it is stable, so hubs with
       equal user counts keep the list's own order. */
    for (i = 1; i < hl->count; i++) {
        hublist_entry tmp = hl->entries[i];

        for (j = i; j > 0 && hl->entries[j - 1].users < tmp.users; j--)
            hl->entries[j] = hl->entries[j - 1];

        hl->entries[j] = tmp;
    }
}
