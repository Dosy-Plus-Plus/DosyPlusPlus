/* browse.c - a peer's file list, parsed once and walked. */
#include "browse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz.h"
#include "../hash/base32.h"

#define ENTS_INITIAL 256
#define POOL_INITIAL 8192

void browse_init(browse_list *b)
{
    memset(b, 0, sizeof *b);
    b->cwd = BROWSE_NO_PARENT;
}

void browse_free(browse_list *b)
{
    free(b->ents);
    free(b->pool);
    memset(b, 0, sizeof *b);
    b->cwd = BROWSE_NO_PARENT;
}

const char *browse_error(const browse_list *b)
{
    return b->err[0] ? b->err : "no error";
}

/* --------------------------------------------------------------- storage */

static unsigned long pool_add(browse_list *b, const char *s)
{
    size_t n = strlen(s) + 1;
    unsigned long off;

    if (!b->pool) {
        b->pool = (char *)malloc(POOL_INITIAL);
        if (!b->pool)
            return 0;
        b->pool_cap = POOL_INITIAL;
        b->pool[0] = '\0';
        b->pool_len = 1;           /* offset 0 is the empty string */
    }

    if (b->pool_len + n > b->pool_cap) {
        unsigned long want = b->pool_cap;
        char *fresh;

        while (want < b->pool_len + n)
            want *= 2;
        fresh = (char *)realloc(b->pool, want);
        if (!fresh)
            return 0;
        b->pool = fresh;
        b->pool_cap = want;
    }

    off = b->pool_len;
    memcpy(b->pool + off, s, n);
    b->pool_len += n;
    return off;
}

static browse_entry *ent_add(browse_list *b, const char *name, long parent,
                             int is_dir)
{
    browse_entry *e;
    unsigned long off;

    if (b->count >= BROWSE_MAX_ENTRIES) {
        b->truncated = 1;
        return NULL;
    }

    if (b->count >= b->cap) {
        long want = b->cap ? b->cap * 2 : ENTS_INITIAL;
        browse_entry *fresh;

        if (want > BROWSE_MAX_ENTRIES)
            want = BROWSE_MAX_ENTRIES;
        fresh = (browse_entry *)realloc(b->ents,
                                        (size_t)want * sizeof *fresh);
        if (!fresh) {
            b->truncated = 1;
            return NULL;
        }
        b->ents = fresh;
        b->cap = want;
    }

    off = pool_add(b, name);
    if (off == 0) {
        b->truncated = 1;
        return NULL;
    }

    e = &b->ents[b->count++];
    memset(e, 0, sizeof *e);
    e->name_off = off;
    e->parent = parent;
    e->is_dir = (unsigned char)(is_dir ? 1 : 0);
    return e;
}

const char *browse_name(const browse_list *b, long index)
{
    if (index < 0 || index >= b->count || !b->pool)
        return "";
    return b->pool + b->ents[index].name_off;
}

/* --------------------------------------------------------------- parsing */

long browse_load(browse_list *b, xp_fill fill, void *user, const char *nick)
{
    xmlpull x;
    long stack[XP_DEPTH_MAX + 2];
    int sp = 0;
    long parent = BROWSE_NO_PARENT;

    browse_free(b);
    browse_init(b);
    snprintf(b->nick, sizeof b->nick, "%s", nick ? nick : "");

    xp_init(&x, fill, user);

    for (;;) {
        xp_event ev = xp_next(&x);

        if (ev == XP_END)
            break;

        if (ev == XP_ERROR) {
            /* Keep whatever was parsed before the break. A truncated list
               is still worth showing, as long as the door says it is
               incomplete rather than pretending it is the whole share. */
            snprintf(b->err, sizeof b->err, "%s", xp_error(&x));
            b->truncated = 1;
            break;
        }

        if (ev == XP_DIR_OPEN) {
            browse_entry *e = ent_add(b, x.name, parent, 1);

            if (!e)
                continue;          /* capped; keep walking to stay balanced */

            e->incomplete = (unsigned char)(x.incomplete ? 1 : 0);
            e->loaded = (unsigned char)(x.incomplete ? 0 : 1);

            if (sp < (int)(sizeof stack / sizeof stack[0])) {
                stack[sp++] = parent;
                parent = b->count - 1;
            }
            continue;
        }

        if (ev == XP_DIR_CLOSE) {
            if (sp > 0)
                parent = stack[--sp];
            else
                parent = BROWSE_NO_PARENT;
            continue;
        }

        if (ev == XP_FILE) {
            browse_entry *e = ent_add(b, x.name, parent, 0);

            if (!e)
                continue;

            e->size = x.size;
            if (x.have_tth &&
                base32_decode(e->tth, sizeof e->tth, x.tth)
                    == TTH_HASH_LEN)
                e->have_tth = 1;
        }
    }

    snprintf(b->base, sizeof b->base, "%s", x.base[0] ? x.base : "/");

    /* A list with nothing marked incomplete is the whole share, however it
       was asked for. That is what lets the door stop saying "fetching" for
       directories that are simply empty. */
    b->whole = 1;
    {
        long i;

        for (i = 0; i < b->count; i++)
            if (b->ents[i].incomplete)
                b->whole = 0;
    }

    b->cwd = BROWSE_NO_PARENT;
    return b->count;
}

/* A list as it arrived, which is not always compressed.
 *
 * files.xml.bz2 is bzip2 by name and by convention. A partial list fetched
 * with `CGET list <dir>` is not: AirDC++ sends it as plain XML, which cost
 * an afternoon to discover because "read error in the file list" is what
 * bzip2 says about perfectly good XML. So the format is sniffed rather
 * than assumed -- a list is either bzip2 or it is text, and the two are
 * distinguishable by their first three bytes.
 */
typedef struct {
    bz_reader *bz;
    FILE      *fp;
} list_source;

static int list_open(list_source *ls, const char *path)
{
    unsigned char magic[3];
    FILE *probe;
    size_t got;

    memset(ls, 0, sizeof *ls);

    probe = fopen(path, "rb");
    if (!probe)
        return -1;
    got = fread(magic, 1, sizeof magic, probe);
    fclose(probe);

    if (got == sizeof magic && magic[0] == 'B' && magic[1] == 'Z' &&
        magic[2] == 'h') {
        ls->bz = bz_open(path);
        return ls->bz ? 0 : -1;
    }

    ls->fp = fopen(path, "rb");
    return ls->fp ? 0 : -1;
}

static void list_close(list_source *ls)
{
    if (ls->bz)
        bz_close(&ls->bz);
    if (ls->fp)
        fclose(ls->fp);
    ls->fp = NULL;
}

static int list_fill(void *user, void *buf, size_t len)
{
    list_source *ls = (list_source *)user;

    if (ls->bz)
        return bz_read(ls->bz, buf, len);
    if (ls->fp) {
        size_t n = fread(buf, 1, len, ls->fp);

        if (n == 0 && ferror(ls->fp))
            return -1;
        return (int)n;
    }
    return -1;
}

/* ------------------------------------------------------- partial lists */

int browse_needs_fetch(const browse_list *b, long index)
{
    if (index < 0 || index >= b->count)
        return 0;
    return b->ents[index].is_dir && !b->ents[index].loaded;
}

void browse_entry_path(const browse_list *b, long index, char *out,
                       size_t outsz)
{
    long chain[XP_DEPTH_MAX + 2];
    int n = 0;
    long at = index;
    size_t used = 0;

    if (outsz == 0)
        return;
    out[0] = '\0';

    while (at >= 0 && at < b->count &&
           n < (int)(sizeof chain / sizeof chain[0])) {
        chain[n++] = at;
        at = b->ents[at].parent;
    }

    if (n == 0) {
        snprintf(out, outsz, "/");
        return;
    }

    while (n-- > 0) {
        int wrote = snprintf(out + used, outsz - used, "/%s",
                             browse_name(b, chain[n]));

        if (wrote < 0 || (size_t)wrote >= outsz - used)
            break;
        used += (size_t)wrote;
    }

    /* A directory is named with a trailing slash on the wire; a file is
       not. Getting this wrong means asking for a directory by a name no
       client recognises. */
    if (index >= 0 && index < b->count && b->ents[index].is_dir &&
        used + 1 < outsz) {
        out[used++] = '/';
        out[used] = '\0';
    }
}

/* Does `base` name the same directory as `want`, allowing for the
   trailing slash and the leading one that clients disagree about? */
static int base_matches(const char *base, const char *want)
{
    size_t bn, wn;

    while (*base == '/')
        base++;
    while (*want == '/')
        want++;

    bn = strlen(base);
    wn = strlen(want);
    while (bn > 0 && base[bn - 1] == '/')
        bn--;
    while (wn > 0 && want[wn - 1] == '/')
        wn--;

    return bn == wn && strncmp(base, want, bn) == 0;
}

long browse_merge_bz2(browse_list *b, const char *path, long parent,
                      const char *expect_base)
{
    list_source ls;
    xmlpull x;
    long stack[XP_DEPTH_MAX + 2];
    int sp = 0;
    long at = parent;
    long added = 0;
    int checked_base = 0;

    if (parent < 0 || parent >= b->count || !b->ents[parent].is_dir) {
        snprintf(b->err, sizeof b->err,
                 "there is nowhere to put that listing");
        return -1;
    }

    /* Already here. Not an error: pressing ENTER twice on a directory
       should not refetch it, and the entries are indices that other
       entries point at, so replacing them would mean renumbering. */
    if (b->ents[parent].loaded)
        return 0;

    if (list_open(&ls, path) != 0) {
        snprintf(b->err, sizeof b->err, "could not read %s", path);
        return -1;
    }

    xp_init(&x, list_fill, &ls);

    for (;;) {
        xp_event ev = xp_next(&x);

        if (ev == XP_END)
            break;

        if (ev == XP_ERROR) {
            snprintf(b->err, sizeof b->err, "%s", xp_error(&x));
            b->truncated = 1;
            break;
        }

        /* The Base says what this listing is of, and it is checked before
           anything is filed: a peer answering a request for one directory
           with a listing of another would otherwise put its files under
           the wrong name. */
        if (!checked_base && x.base[0]) {
            checked_base = 1;
            if (expect_base && expect_base[0] &&
                !base_matches(x.base, expect_base)) {
                snprintf(b->err, sizeof b->err,
                         "the listing is of %s, not %s", x.base,
                         expect_base);
                list_close(&ls);
                return -1;
            }
        }

        if (ev == XP_DIR_OPEN) {
            browse_entry *e = ent_add(b, x.name, at, 1);

            if (!e)
                continue;

            e->incomplete = (unsigned char)(x.incomplete ? 1 : 0);
            e->loaded = (unsigned char)(x.incomplete ? 0 : 1);
            added++;

            if (sp < (int)(sizeof stack / sizeof stack[0])) {
                stack[sp++] = at;
                at = b->count - 1;
            }
            continue;
        }

        if (ev == XP_DIR_CLOSE) {
            at = (sp > 0) ? stack[--sp] : parent;
            continue;
        }

        if (ev == XP_FILE) {
            browse_entry *e = ent_add(b, x.name, at, 0);

            if (!e)
                continue;

            e->size = x.size;
            if (x.have_tth &&
                base32_decode(e->tth, sizeof e->tth, x.tth)
                    == TTH_HASH_LEN)
                e->have_tth = 1;
            added++;
        }
    }

    list_close(&ls);

    /* Loaded even if it turned out to be empty: an empty directory that
       keeps asking the peer about itself is worse than one that says it is
       empty. */
    b->ents[parent].loaded = 1;
    b->ents[parent].incomplete = 0;
    return added;
}

long browse_load_bz2(browse_list *b, const char *path, const char *nick)
{
    list_source ls;
    long n;

    /* Compressed or not: the same sniffing as the merge path, for the same
       reason. A root listing fetched with `CGET list /` arrives as plain
       XML from AirDC++ and as bzip2 from us. */
    if (list_open(&ls, path) != 0) {
        browse_free(b);
        browse_init(b);
        snprintf(b->err, sizeof b->err, "could not read %s", path);
        return -1;
    }

    n = browse_load(b, list_fill, &ls, nick);
    list_close(&ls);

    if (n == 0 && b->err[0])
        return -1;
    return n;
}

/* ------------------------------------------------------------ navigation */

void browse_enter(browse_list *b, long index)
{
    if (index < 0 || index >= b->count)
        return;
    if (!b->ents[index].is_dir)
        return;
    b->cwd = index;
}

void browse_up(browse_list *b)
{
    if (b->cwd < 0 || b->cwd >= b->count)
        return;
    b->cwd = b->ents[b->cwd].parent;
}

long browse_listing(browse_list *b, long *out, long max)
{
    long n = 0;
    long i;
    int pass;

    if (max <= 0)
        return 0;

    /* Directories first, as every file manager since Norton Commander.
       Two passes rather than a sort: the table is already in list order,
       which is the order the sharer chose. */
    for (pass = 0; pass < 2 && n < max; pass++)
        for (i = 0; i < b->count && n < max; i++)
            if (b->ents[i].parent == b->cwd &&
                (int)b->ents[i].is_dir == (pass == 0))
                out[n++] = i;

    return n;
}

void browse_path(const browse_list *b, char *out, size_t outsz)
{
    long chain[XP_DEPTH_MAX + 2];
    int n = 0;
    long at = b->cwd;
    size_t used = 0;

    if (outsz == 0)
        return;
    out[0] = '\0';

    while (at >= 0 && at < b->count &&
           n < (int)(sizeof chain / sizeof chain[0])) {
        chain[n++] = at;
        at = b->ents[at].parent;
    }

    if (n == 0) {
        snprintf(out, outsz, "/");
        return;
    }

    while (n-- > 0) {
        const char *name = browse_name(b, chain[n]);
        int wrote = snprintf(out + used, outsz - used, "/%s", name);

        if (wrote < 0 || (size_t)wrote >= outsz - used) {
            /* Deep enough that the path does not fit. Truncation is fine
               here; it is a header, not an identifier. */
            out[outsz - 1] = '\0';
            return;
        }
        used += (size_t)wrote;
    }
}

int browse_tth(const browse_list *b, long index, char *out, size_t outsz)
{
    if (index < 0 || index >= b->count)
        return -1;
    if (!b->ents[index].have_tth || b->ents[index].is_dir)
        return -1;
    if (outsz < BASE32_TIGER_LEN + 1)
        return -1;

    base32_encode(out, outsz, b->ents[index].tth, TTH_HASH_LEN);
    return 0;
}
