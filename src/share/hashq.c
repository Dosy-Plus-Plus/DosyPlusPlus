/* hashq.c - byte-budgeted hashing with a persistent cache. */
#include "hashq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hashq {
    share_index *ix;

    share_file  *cur;               /* file being hashed, or NULL */
    FILE        *fp;
    tth_ctx      ctx;
    tth_u64      cur_done;

    tth_u64      done_bytes;        /* across the whole run */
    char         err[160];
    char         current[128];
};

int hashq_init(hashq **hqp, share_index *ix)
{
    hashq *hq = (hashq *)calloc(1, sizeof *hq);

    *hqp = NULL;
    if (!hq)
        return -1;
    hq->ix = ix;
    *hqp = hq;
    return 0;
}

void hashq_free(hashq **hqp)
{
    hashq *hq = *hqp;

    if (!hq)
        return;
    if (hq->fp)
        fclose(hq->fp);
    free(hq);
    *hqp = NULL;
}

const char *hashq_current(const hashq *hq)
{
    return hq ? hq->current : "";
}

tth_u64 hashq_done_bytes(const hashq *hq)
{
    return hq ? hq->ix->hashed_size + hq->cur_done : 0;
}

tth_u64 hashq_total_bytes(const hashq *hq)
{
    return hq ? hq->ix->total_size : 0;
}

unsigned int hashq_files_left(const hashq *hq)
{
    if (!hq)
        return 0;
    return hq->ix->count - hq->ix->hashed_count;
}

int hashq_percent(const hashq *hq)
{
    tth_u64 total, done;

    if (!hq || hq->ix->total_size == 0)
        return 100;

    total = hq->ix->total_size;
    done = hashq_done_bytes(hq);

    /* Integer only -- the program is built -msoft-float. Scale down first
       so a multi-gigabyte share cannot overflow the multiply. */
    while (total > 0xFFFFFFUL) {
        total >>= 4;
        done >>= 4;
    }
    if (total == 0)
        return 100;
    return (int)((done * 100) / total);
}

const char *hashq_error(const hashq *hq)
{
    return (hq && hq->err[0]) ? hq->err : "no error";
}

void hashq_reset(hashq *hq)
{
    if (!hq)
        return;

    if (hq->fp) {
        fclose(hq->fp);
        hq->fp = NULL;
    }
    hq->cur = NULL;
    hq->cur_done = 0;

    /* The index owns the hash table that goes with those hashes, so it
       does the forgetting -- clearing the flags here and leaving the table
       populated would hand out records that no longer claim a hash. */
    share_unhash_all(hq->ix);
}

/* ------------------------------------------------------------- cache */

int hashq_load(hashq *hq, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int hits = 0;

    if (!f)
        return 0;                   /* first run: not an error */

    while (fgets(line, sizeof line, f)) {
        char b32[64];
        unsigned long size_lo, mtime;
        char vpath[256];
        unsigned int i;
        unsigned char raw[TTH_HASH_LEN + 4];

        /* <tth-base32> <size> <mtime> <virtual path with spaces> */
        if (sscanf(line, "%63s %lu %lu %255[^\n]",
                   b32, &size_lo, &mtime, vpath) != 4)
            continue;               /* a corrupt line is skipped, not fatal */

        if (base32_decode(raw, sizeof raw, b32) != TTH_HASH_LEN)
            continue;

        for (i = 0; i < hq->ix->count; i++) {
            share_file *sf = &hq->ix->files[i];

            if (sf->hashed)
                continue;
            if (strcmp(share_virt(hq->ix, sf), vpath) != 0)
                continue;

            /* Size and timestamp must both match. Either changing means the
               file is different and the cached hash is wrong -- serving a
               stale hash would fail verification at the far end. */
            if ((unsigned long)sf->size != size_lo || sf->mtime != mtime)
                break;

            share_set_tth(hq->ix, sf, raw);
            hits++;
            break;
        }
    }

    fclose(f);
    return hits;
}

int hashq_save(hashq *hq, const char *path)
{
    FILE *f;
    unsigned int i;

    if (!hq)
        return -1;

    f = fopen(path, "w");
    if (!f) {
        snprintf(hq->err, sizeof hq->err,
                 "cannot write %s -- the share will be rehashed next time",
                 path);
        return -1;
    }

    fprintf(f, "; DOSy++ hash cache. Delete to force a rehash.\n");
    fprintf(f, "; <tth> <size> <mtime> <path>\n");

    for (i = 0; i < hq->ix->count; i++) {
        const share_file *sf = &hq->ix->files[i];
        char b32[BASE32_TIGER_LEN + 1];

        if (!sf->hashed)
            continue;

        base32_encode(b32, sizeof b32, sf->tth, TTH_HASH_LEN);
        fprintf(f, "%s %lu %lu %s\n", b32, (unsigned long)sf->size,
                sf->mtime, share_virt(hq->ix, sf));
    }

    if (fclose(f) != 0) {
        snprintf(hq->err, sizeof hq->err, "error closing %s", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ hashing */

static void finish_current(hashq *hq)
{
    unsigned char root[TTH_HASH_LEN];

    tth_final(&hq->ctx, root);
    share_set_tth(hq->ix, hq->cur, root);

    fclose(hq->fp);
    hq->fp = NULL;
    hq->cur = NULL;
    hq->cur_done = 0;
}

static void abandon_current(hashq *hq, const char *why)
{
    snprintf(hq->err, sizeof hq->err, "%s: %s",
             hq->cur ? share_virt(hq->ix, hq->cur) : "?", why);

    /* Mark it hashed with a zero hash? No: that would advertise a file we
       cannot serve. Instead drop it from the accounting by giving it the
       hash of nothing and leaving it unhashed, so share_match skips it. */
    if (hq->fp) {
        fclose(hq->fp);
        hq->fp = NULL;
    }
    hq->cur = NULL;
    hq->cur_done = 0;
}

int hashq_tick(hashq *hq, unsigned long budget)
{
    unsigned char buf[8192];
    unsigned long spent = 0;

    if (!hq)
        return 0;

    while (spent < budget) {
        size_t want, got;

        if (!hq->cur) {
            hq->cur = share_next_unhashed(hq->ix);
            if (!hq->cur)
                return 0;           /* complete */

            snprintf(hq->current, sizeof hq->current, "%s",
                     share_virt(hq->ix, hq->cur));

            hq->fp = fopen(share_local(hq->ix, hq->cur), "rb");
            if (!hq->fp) {
                /* Vanished or unreadable between the scan and now -- which
                   happens, and must not stop the queue. */
                abandon_current(hq, "cannot open");
                return -1;
            }
            tth_init(&hq->ctx, NULL, NULL, -1);
            hq->cur_done = 0;
        }

        want = sizeof buf;
        if (want > budget - spent)
            want = (size_t)(budget - spent);

        got = fread(buf, 1, want, hq->fp);
        if (got == 0) {
            if (ferror(hq->fp)) {
                abandon_current(hq, "read error");
                return -1;
            }
            /* Clean end of file. */
            finish_current(hq);
            continue;
        }

        tth_update(&hq->ctx, buf, got);
        hq->cur_done += (tth_u64)got;
        hq->done_bytes += (tth_u64)got;
        spent += (unsigned long)got;

        /* A short read that is not an error means end of file on some
           platforms; loop round and let fread return 0 to confirm rather
           than guessing. */
    }

    return 1;                       /* more to do */
}
