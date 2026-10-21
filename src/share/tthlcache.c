/* tthlcache.c - the leaf-hash store. See tthlcache.h. */
#include "tthlcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "DTTL" and a format byte. A version rather than a bare magic because the
   record layout is the sort of thing that changes once: an unknown version
   is treated as an empty cache and overwritten, which costs one rehash. */
#define TTHL_MAGIC   "DTTL"
#define TTHL_VERSION 1
#define TTHL_HEADER  5

/* Leaves per record, matching what share_write_tthl will produce. A record
   claiming more than this came from somewhere else. */
#define TTHL_MAX_LEAVES 1024

/* One record's fixed part: hash, level, count. */
#define TTHL_REC_HEAD (TTH_HASH_LEN + 1 + 2)

static int read_u16(FILE *f, unsigned *out)
{
    int lo = fgetc(f);
    int hi = fgetc(f);

    if (lo == EOF || hi == EOF)
        return -1;
    *out = (unsigned)lo | ((unsigned)hi << 8);
    return 0;
}

/* Open the store for reading and step past the header. Returns NULL if
   there is no usable store, which includes a header this build does not
   recognise. */
static FILE *open_for_read(const char *store)
{
    FILE *f = fopen(store, "rb");
    char magic[4];
    int ver;

    if (!f)
        return NULL;

    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, TTHL_MAGIC, 4) != 0) {
        fclose(f);
        return NULL;
    }
    ver = fgetc(f);
    if (ver != TTHL_VERSION) {
        fclose(f);
        return NULL;
    }
    return f;
}

/* Find a record and leave the handle sitting at its payload. Returns the
   open file, with *payload set to the number of bytes of leaves, or NULL if
   the hash is not in the store. The caller closes it.
 *
   Shared by the lookup and the insert, because "is it already there?" and
   "give me what is there" are the same scan and keeping two of them would
   be two chances to read the format differently. */
static FILE *find_record(const char *store, const unsigned char *tth,
                         int *level_out, long *payload)
{
    FILE *f = open_for_read(store);
    unsigned char key[TTH_HASH_LEN];
    int level;
    unsigned count;

    if (!f)
        return NULL;

    for (;;) {
        long bytes;

        if (fread(key, 1, TTH_HASH_LEN, f) != TTH_HASH_LEN)
            break;                      /* end of the store */

        level = fgetc(f);
        if (level < 0 || read_u16(f, &count) != 0)
            break;                      /* truncated: stop, do not guess */

        if (count == 0 || count > TTHL_MAX_LEAVES)
            break;                      /* not a record we wrote */

        bytes = (long)count * TTH_HASH_LEN;

        if (memcmp(key, tth, TTH_HASH_LEN) != 0) {
            if (fseek(f, bytes, SEEK_CUR) != 0)
                break;
            continue;
        }

        if (level_out)
            *level_out = level;
        if (payload)
            *payload = bytes;
        return f;
    }

    fclose(f);
    return NULL;
}

long tthl_cache_get(const char *store, const unsigned char *tth,
                    const char *out_path, int *level_out)
{
    long payload = 0;
    int level = 0;
    FILE *f = find_record(store, tth, &level, &payload);

    if (!f)
        return -1;

    {
        /* Copied out to a file because that is what the sender wants: it
           serves leaves the same way it serves anything else. */
        {
            FILE *out = fopen(out_path, "wb");
            unsigned char buf[4096];
            long left = payload;

            if (!out) {
                fclose(f);
                return -1;
            }

            while (left > 0) {
                size_t want = sizeof buf;
                size_t got;

                if ((long)want > left)
                    want = (size_t)left;

                got = fread(buf, 1, want, f);
                if (got == 0)
                    break;
                if (fwrite(buf, 1, got, out) != got) {
                    fclose(out);
                    fclose(f);
                    remove(out_path);
                    return -1;
                }
                left -= (long)got;
            }

            fclose(out);
            fclose(f);

            if (left != 0) {
                /* The store is truncated. Treat it as a miss rather than
                   serving half a leaf set, which would fail at the far end
                   with no explanation. */
                remove(out_path);
                return -1;
            }

            if (level_out)
                *level_out = level;
            return payload;
        }
    }
}

/* Drop the oldest records, keeping the newest half.
 *
 * Rewriting the file is the whole of the eviction policy. The store is
 * half a megabyte, this happens rarely, and a scheme with free lists would
 * be more code than the problem deserves. The kept half is held in memory
 * for the moment it takes -- at most half the cap, so 256KB. */
static void evict_half(const char *store)
{
    FILE *f = open_for_read(store);
    long offsets[256];
    long sizes[256];
    int n = 0;
    int i;
    int keep_from;
    long keep_bytes = 0;
    unsigned char *kept;
    long at = 0;

    if (!f)
        return;

    while (n < (int)(sizeof offsets / sizeof offsets[0])) {
        unsigned char key[TTH_HASH_LEN];
        int level;
        unsigned count;
        long here = ftell(f);

        if (here < 0)
            break;
        if (fread(key, 1, TTH_HASH_LEN, f) != TTH_HASH_LEN)
            break;
        level = fgetc(f);
        if (level < 0 || read_u16(f, &count) != 0)
            break;
        if (count == 0 || count > TTHL_MAX_LEAVES)
            break;

        offsets[n] = here;
        sizes[n] = TTHL_REC_HEAD + (long)count * TTH_HASH_LEN;
        n++;

        if (fseek(f, (long)count * TTH_HASH_LEN, SEEK_CUR) != 0)
            break;
    }

    if (n <= 1) {
        /* One record filling the cache on its own: there is nothing to
           keep half of, so start again. */
        fclose(f);
        remove(store);
        return;
    }

    keep_from = n / 2;
    for (i = keep_from; i < n; i++)
        keep_bytes += sizes[i];

    kept = (unsigned char *)malloc((size_t)keep_bytes);
    if (!kept) {
        /* No memory to shuffle it: dropping the cache entirely is worse
           than leaving it over the cap for now, so leave it alone. */
        fclose(f);
        return;
    }

    for (i = keep_from; i < n; i++) {
        if (fseek(f, offsets[i], SEEK_SET) != 0)
            break;
        if (fread(kept + at, 1, (size_t)sizes[i], f) != (size_t)sizes[i])
            break;
        at += sizes[i];
    }
    fclose(f);

    {
        FILE *out = fopen(store, "wb");

        if (out) {
            fwrite(TTHL_MAGIC, 1, 4, out);
            fputc(TTHL_VERSION, out);
            fwrite(kept, 1, (size_t)at, out);
            fclose(out);
        }
    }

    free(kept);
}

int tthl_cache_put(const char *store, const unsigned char *tth, int level,
                   const unsigned char *leaves, long bytes)
{
    FILE *f;
    long count;
    int fresh = 0;

    if (!leaves || bytes <= 0 || bytes % TTH_HASH_LEN != 0)
        return -1;

    count = bytes / TTH_HASH_LEN;
    if (count > TTHL_MAX_LEAVES)
        return -1;

    /* Already there? Being asked twice in a row is the common case, and a
       second copy would just waste the space. */
    {
        FILE *have = find_record(store, tth, NULL, NULL);

        if (have) {
            fclose(have);
            return 0;
        }
    }

    {
        long size = 0;

        if (tthl_cache_stat(store, &size) >= 0 &&
            size + TTHL_REC_HEAD + bytes > (long)TTHL_CACHE_MAX_BYTES)
            evict_half(store);
    }

    f = fopen(store, "r+b");
    if (!f) {
        f = fopen(store, "wb");
        fresh = 1;
        if (!f)
            return -1;
    } else {
        /* An existing file with a header we do not know is replaced: one
           rehash is cheaper than guessing at somebody else's format. */
        char magic[4];
        int ver;

        if (fread(magic, 1, 4, f) != 4 ||
            memcmp(magic, TTHL_MAGIC, 4) != 0 ||
            (ver = fgetc(f)) != TTHL_VERSION) {
            fclose(f);
            f = fopen(store, "wb");
            fresh = 1;
            if (!f)
                return -1;
        }
    }

    if (fresh) {
        if (fwrite(TTHL_MAGIC, 1, 4, f) != 4 ||
            fputc(TTHL_VERSION, f) == EOF) {
            fclose(f);
            return -1;
        }
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    if (fwrite(tth, 1, TTH_HASH_LEN, f) != TTH_HASH_LEN ||
        fputc(level & 0xFF, f) == EOF ||
        fputc((int)(count & 0xFF), f) == EOF ||
        fputc((int)((count >> 8) & 0xFF), f) == EOF ||
        fwrite(leaves, 1, (size_t)bytes, f) != (size_t)bytes) {
        fclose(f);
        return -1;
    }

    if (fclose(f) != 0)
        return -1;
    return 0;
}

int tthl_cache_stat(const char *store, long *bytes_out)
{
    FILE *f = open_for_read(store);
    int n = 0;

    if (bytes_out)
        *bytes_out = 0;

    if (!f)
        return -1;

    for (;;) {
        unsigned char key[TTH_HASH_LEN];
        int level;
        unsigned count;

        if (fread(key, 1, TTH_HASH_LEN, f) != TTH_HASH_LEN)
            break;
        level = fgetc(f);
        if (level < 0 || read_u16(f, &count) != 0)
            break;
        if (count == 0 || count > TTHL_MAX_LEAVES)
            break;
        if (fseek(f, (long)count * TTH_HASH_LEN, SEEK_CUR) != 0)
            break;
        n++;
    }

    if (bytes_out) {
        long end = ftell(f);

        *bytes_out = (end > 0) ? end : 0;
    }

    fclose(f);
    return n;
}
