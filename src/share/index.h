/* index.h - what we are sharing.
 *
 * One record per file, and the records have to be small: the plan's target
 * is a 10,000-file share on a machine with 16MB. Two decisions follow:
 *
 *   - Paths are interned in a string pool and referenced by offset. Share
 *     trees are deep and repetitive ("warez/apps/" prefixes a hundred
 *     files), so storing a full path per record would cost more than
 *     everything else combined.
 *   - The TTH is kept as 24 raw bytes, not the 40-character base32 form.
 *     Base32 is a presentation detail; converting on demand costs nothing
 *     and saves 16 bytes per file.
 *
 * The result is 56 bytes per file on DOS, so 10,000 files is well under a
 * megabyte plus the pool. (The host build is wider, because `long` is 8
 * bytes there; the 16MB budget is a DOS concern and `make dos-bench-run`
 * prints the real figure.) The index itself is heap-allocated -- it is far too big for
 * the stack, and an earlier version of the search table taught that lesson.
 *
 * Two lookups have to be fast, because a busy hub asks constantly:
 *
 *   - By hash (`TR` searches, and every incoming CGET). An open-addressed
 *     table keyed on the TTH makes that a probe instead of a scan over
 *     every file.
 *   - By name (`AN` terms). A substring search cannot be indexed, so each
 *     record carries a bitmask of which letters appear in its path: a term
 *     containing a letter the file does not have is rejected without
 *     touching the string at all. That turns most of a 10,000-file scan
 *     into 10,000 bitwise ANDs.
 *
 * No DOS dependency: matching and accounting are pure computation and are
 * unit tested. The directory walk lives in scan.c, which is where the
 * platform difference is.
 */
#ifndef DOSYPP_SHARE_INDEX_H
#define DOSYPP_SHARE_INDEX_H

#include <stddef.h>

#include "../hash/tth.h"
#include "../adc/search.h"

#define SHARE_MAX_ROOTS 8
#define SHARE_ROOT_MAX  128     /* a local path, e.g. C:\DL */

typedef struct {
    unsigned long name_off;     /* basename, in the pool */
    unsigned long virt_off;     /* virtual path we advertise, in the pool */
    unsigned long local_off;    /* local path to open, in the pool */
    tth_u64       size;
    unsigned long mtime;
    unsigned char tth[TTH_HASH_LEN];
    unsigned char hashed;

    /* Which letters appear anywhere in the virtual path: bit 0 for 'a'
       through bit 25 for 'z', bit 26 for any digit. A search term can only
       match if every letter it needs is present, so this rejects most
       files before any string comparison. On DOS it costs nothing at all:
       the record was already padded to 56 bytes and this fits in the
       padding. */
    unsigned long letters;
} share_file;

typedef struct {
    share_file   *files;
    unsigned int  count;
    unsigned int  cap;

    char         *pool;         /* interned strings, NUL separated */
    unsigned long pool_len;
    unsigned long pool_cap;

    /* The roots being shared, as configured. */
    char          roots[SHARE_MAX_ROOTS][SHARE_ROOT_MAX];
    int           nroot;

    /* Accounting, kept live so INF updates and the share door are cheap. */
    tth_u64       total_size;
    unsigned int  hashed_count;
    tth_u64       hashed_size;

    /* Hash -> record. 0 means free, anything else is slot+1. Power of two,
       so the probe is a mask rather than a modulo. No tombstones are needed
       because a hash is only ever added: files are removed by clearing the
       whole index, never one at a time. */
    unsigned long *tth_idx;
    unsigned int   tth_cap;
    unsigned int   tth_used;
} share_index;

int  share_init(share_index *ix);
void share_free(share_index *ix);
void share_clear(share_index *ix);

/* Add a root. Returns 0, or -1 if the table is full or the path is too
   long. */
int share_add_root(share_index *ix, const char *path);

/* Record a file. `virt` is the path we advertise to the hub (forward
   slashes, root name first); `local` is what we open. Returns the new
   entry, or NULL if memory ran out. */
share_file *share_add_file(share_index *ix, const char *virt,
                           const char *local, tth_u64 size,
                           unsigned long mtime);

/* Pool accessors. Never return NULL: an invalid offset yields "". */
const char *share_str(const share_index *ix, unsigned long off);
const char *share_name(const share_index *ix, const share_file *f);
const char *share_virt(const share_index *ix, const share_file *f);
const char *share_local(const share_index *ix, const share_file *f);

/* Attach a hash to a file and update the accounting. Also indexes it, so
   the next lookup by hash is a probe. */
void share_set_tth(share_index *ix, share_file *f,
                   const unsigned char *tth);

/* Forget every hash, keeping the file list (the /rehash path). The hash
   index is emptied with them -- leaving it populated would hand out records
   that no longer claim to be hashed. */
void share_unhash_all(share_index *ix);

/* Find by hash. Only hashed files can match, which is the point: we must
   not claim to have a file we cannot name by hash. */
share_file *share_find_tth(share_index *ix, const unsigned char *tth);
share_file *share_find_tth_b32(share_index *ix, const char *b32);

/* The next file still needing a hash, or NULL when the share is complete.
   Returned in index order so progress is monotonic and resumable. */
share_file *share_next_unhashed(share_index *ix);

/* --- matching an incoming search -------------------------------------
 *
 * Fills `out` with up to `max` matching files. Unhashed files are skipped:
 * answering a search with a file we cannot address by hash would advertise
 * something we cannot serve.
 *
 * Returns the number of matches written. */
int share_match(share_index *ix, const adc_msg *m, share_file **out,
                int max);

/* Write the leaf hashes of a shared file to `path`, as the raw
   concatenation ADC's `tthl` data is: 24 bytes per node, in file order.
 *
 * The level is chosen so the result stays small enough to serve from a DOS
 * machine -- level 0 on a 1GB file would be 24MB of hashes. Returns the
 * number of bytes written, or -1. `*level_out` receives the level used,
 * which is not sent on the wire but is worth having in a log.
 *
 * This re-reads the file, so it costs a hash pass: about two seconds per
 * megabyte on a 486. Real clients keep their leaves in a database; we do
 * not have one, and inventing it is a bigger change than this.
 */
long share_write_tthl(share_index *ix, share_file *f, const char *path,
                      int *level_out);

/* How many results to offer one search. The convention clients follow is
   10 for an active search and 5 for a passive one; more than that is
   considered flooding and some hubs act on it. */
#define SHARE_MAX_REPLIES_ACTIVE  10
#define SHARE_MAX_REPLIES_PASSIVE 5

#endif /* DOSYPP_SHARE_INDEX_H */
