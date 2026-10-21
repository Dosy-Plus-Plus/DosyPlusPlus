/* hashq.h - hashing the share, and not doing it twice.
 *
 * Hashing is the expensive part of this program: measured at 517 KB/s on a
 * cycle-limited 486DX2-66, which is about 33 minutes per gigabyte. Two
 * things follow:
 *
 *   - It must be interruptible. hashq_tick() consumes a byte budget and
 *     returns, so the UI redraws and the hub connection keeps being
 *     serviced. The budget is in bytes rather than milliseconds because
 *     that is deterministic and therefore testable; the caller picks a
 *     budget from the throughput it wants to spend.
 *
 *   - It must never be repeated unnecessarily. Hashes are cached on disk
 *     against (path, size, mtime). A file whose size and timestamp are
 *     unchanged is taken from the cache, so a restart costs seconds rather
 *     than the whole share again. That is the difference between a client
 *     you can restart and one you cannot.
 *
 * The cache is a plain text file -- DOSYPP.TTH, one record per line. It is
 * a cache, not a source of truth: losing it costs time, not correctness,
 * and a corrupt line is skipped rather than fatal.
 */
#ifndef DOSYPP_SHARE_HASHQ_H
#define DOSYPP_SHARE_HASHQ_H

#include "index.h"

#define HASHQ_FILE "DOSYPP.TTH"

/* A sensible slice. At the measured 517 KB/s this is about an eighth of a
   second of work, which keeps a lightbar responsive while hashing. */
#define HASHQ_BUDGET 65536

typedef struct hashq hashq;

int  hashq_init(hashq **hq, share_index *ix);
void hashq_free(hashq **hq);

/* Load cached hashes and attach them to matching files. Returns the number
   of files satisfied from the cache. Missing file is not an error. */
int hashq_load(hashq *hq, const char *path);

/* Write the cache back. Returns 0, or -1 if it could not be written --
   worth reporting, because the next start will rehash everything. */
int hashq_save(hashq *hq, const char *path);

/* Hash up to `budget` bytes. Returns:
     1  more work remains
     0  the share is fully hashed
    -1  a file could not be read; it is skipped and the error recorded  */
int hashq_tick(hashq *hq, unsigned long budget);

/* Progress, for the gauge. */
const char   *hashq_current(const hashq *hq);
tth_u64       hashq_done_bytes(const hashq *hq);
tth_u64       hashq_total_bytes(const hashq *hq);
unsigned int  hashq_files_left(const hashq *hq);
int           hashq_percent(const hashq *hq);
const char   *hashq_error(const hashq *hq);

/* Throw away every hash and start again (the /rehash command). */
void hashq_reset(hashq *hq);

#endif /* DOSYPP_SHARE_HASHQ_H */
