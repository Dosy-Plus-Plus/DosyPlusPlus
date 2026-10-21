/* tthlcache.h - keeping the leaf hashes we have already computed.
 *
 * A peer asking for `tthl` wants the Merkle tree's intermediate hashes. We
 * have no store of them, so the first version recomputed them from the file
 * on every request: a full hash pass, about two seconds per megabyte on a
 * 486. Two peers asking for the same 50MB file meant three minutes of disk
 * for data that had not changed.
 *
 * This is that store. One file, DOSYPP.TTL, holding records of
 *
 *     [ tth (24 raw bytes) | level (1) | count (2, little endian) |
 *       leaves (count * 24) ]
 *
 * after a short header. Keyed by the root hash, which makes it
 * self-invalidating: if the file on disk changes, its root changes, and the
 * old record is simply never asked for again. Nothing has to notice an
 * edit.
 *
 * Bounded, because leaves are bulky -- 24KB for a file at the cap -- and a
 * thousand-file share would otherwise grow a 24MB cache next to a 16MB
 * machine. When the store is full the oldest half is dropped: a cache that
 * refuses to grow and never forgets would stop helping the moment somebody
 * added files.
 */
#ifndef DOSYPP_SHARE_TTHLCACHE_H
#define DOSYPP_SHARE_TTHLCACHE_H

#include <stddef.h>

#include "../hash/tth.h"

#define TTHL_CACHE_FILE "DOSYPP.TTL"

/* How big the store may get before the oldest half is dropped. 512KB is
   twenty or so files' worth of leaves, which covers a share whose popular
   files are a handful, and is a rounding error on a 16MB machine. */
#define TTHL_CACHE_MAX_BYTES (512UL * 1024UL)

/* Write the cached leaves for `tth` into `out_path`. Returns the number of
   bytes written and sets *level_out, or -1 if they are not cached (which is
   not an error -- it means compute them).
 *
 * `tth` is the 24 raw bytes of the root, not base32. */
long tthl_cache_get(const char *store, const unsigned char *tth,
                    const char *out_path, int *level_out);

/* Remember these leaves. Returns 0, or -1 if the store could not be
   written -- which is survivable: the cache is a cache, and the caller can
   always recompute. */
int tthl_cache_put(const char *store, const unsigned char *tth, int level,
                   const unsigned char *leaves, long bytes);

/* How many records the store holds, and how big it is. For the share door
   and for tests; counting means reading the headers, so it is not free. */
int tthl_cache_stat(const char *store, long *bytes_out);

#endif /* DOSYPP_SHARE_TTHLCACHE_H */
