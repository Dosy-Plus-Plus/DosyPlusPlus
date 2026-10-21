/* bz.h - bzip2, as much of it as a file list needs.
 *
 * An ADC file list is `files.xml.bz2`. The compression is not optional: a
 * peer asking for a list expects bzip2 and will not parse anything else, so
 * browse works in neither direction without this.
 *
 * libbzip2 is vendored (tools/fetchbzip2.sh) rather than reimplemented. The
 * wrapper exists for three reasons:
 *
 *   - Both directions are streaming. A file list is the one thing here that
 *     has no bound: someone with 200,000 files has a 20MB list, and
 *     decompressing that into memory on a 16MB machine is not an option. So
 *     compression reads a file and writes a file, and decompression hands
 *     out bytes as the parser asks for them.
 *   - Memory is chosen deliberately, and differently on DOS. See
 *     BZ_BLOCK_100K and the `small` note in bz.c.
 *   - libbzip2 reports errors as negative codes in an out-parameter, which
 *     is easy to ignore by accident. Everything here returns 0 or -1 and
 *     leaves a sentence behind.
 */
#ifndef DOSYPP_BZ_H
#define DOSYPP_BZ_H

#include <stddef.h>

/* Compress `src` to `dst`, replacing it. Returns 0, or -1 with the reason
   available from bz_error(). */
int bz_compress_file(const char *src, const char *dst);

/* --- streaming decompression --- */

typedef struct bz_reader bz_reader;

/* Open a .bz2 file for reading. Returns NULL on failure (bz_error()). */
bz_reader *bz_open(const char *path);

/* Read up to `len` decompressed bytes. Returns the number read, 0 at the
   end of the stream, or -1 on error -- which includes a truncated or
   corrupt stream, because a file list arrives from a stranger. */
int bz_read(bz_reader *r, void *buf, size_t len);

void bz_close(bz_reader **r);

/* The last failure, as a sentence. Never NULL. */
const char *bz_error(void);

#endif /* DOSYPP_BZ_H */
