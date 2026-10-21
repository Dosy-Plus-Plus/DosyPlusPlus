/* bz.c - the bzip2 wrapper. See bz.h for why this exists.
 *
 * libbzip2 is compiled with -DBZ_NO_STDIO, for two reasons that turned out
 * to be the same reason:
 *
 *   - It removes the library's own FILE* layer, so the buffering is ours
 *     and there is one less thing doing I/O behind our back on DOS.
 *   - It removes the verbose progress printing, which is the only floating
 *     point anywhere in libbzip2 ("ratio %5.2f"). DJGPP has no soft-float
 *     library, so with -msoft-float those few printf arguments fail the
 *     link with __divdf3 and friends undefined. Nothing we call would ever
 *     have executed them -- verbosity is 0 -- but they still have to not
 *     be there.
 *
 * So this talks to the low-level stream API, which is the part of libbzip2
 * that does nothing but transform buffers.
 */
#include "bz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bzlib.h"

/* Compression block size, in hundreds of kilobytes.
 *
 * libbzip2 needs 400KB + 8 x blockSize for compression, so 1 costs about
 * 1.2MB and 9 about 7.6MB. On a 16MB DOS machine already holding a share
 * index, 7.6MB is not available and the difference it buys on XML is a few
 * per cent. The format records the block size, so any decompressor copes. */
#define BZ_BLOCK_100K 1

#define BZ_CHUNK 8192

static char g_err[160];

/* BZ_NO_STDIO makes this the caller's job: libbzip2 calls it when an
   internal invariant fails, and it must not return. There is nothing
   sensible to do about it -- the alternative is carrying on with a
   corrupted compressor -- but it should say so rather than vanish. */
void bz_internal_error(int errcode)
{
    fprintf(stdout, "\nbzip2 internal error %d -- this is a bug\n", errcode);
    fflush(stdout);
    exit(3);
}

static void seterr(const char *what, int bzcode)
{
    /* bzip2's codes are worth translating: "-5" in a status line tells
       nobody anything, and the interesting ones are distinguishable. */
    const char *why;

    switch (bzcode) {
    case BZ_OK: case BZ_RUN_OK: case BZ_FLUSH_OK: case BZ_FINISH_OK:
        why = NULL; break;
    case BZ_MEM_ERROR:      why = "out of memory"; break;
    case BZ_DATA_ERROR:     why = "the compressed data is corrupt"; break;
    case BZ_DATA_ERROR_MAGIC:
        why = "not a bzip2 stream"; break;
    case BZ_UNEXPECTED_EOF: why = "the stream is truncated"; break;
    case BZ_CONFIG_ERROR:   why = "libbzip2 is misconfigured for this "
                                  "platform"; break;
    case BZ_PARAM_ERROR:    why = "bad parameters (a bug here)"; break;
    case BZ_SEQUENCE_ERROR: why = "calls out of sequence (a bug here)";
                            break;
    default:                why = "unknown bzip2 failure"; break;
    }

    if (why)
        snprintf(g_err, sizeof g_err, "%s: %s", what, why);
    else
        snprintf(g_err, sizeof g_err, "%s", what);
}

const char *bz_error(void)
{
    return g_err[0] ? g_err : "no error";
}

/* ------------------------------------------------------------- compress */

int bz_compress_file(const char *src, const char *dst)
{
    FILE *in = NULL, *out = NULL;
    bz_stream s;
    char inbuf[BZ_CHUNK];
    char outbuf[BZ_CHUNK];
    int started = 0;
    int rc;

    g_err[0] = '\0';

    in = fopen(src, "rb");
    if (!in) {
        snprintf(g_err, sizeof g_err, "cannot read %s", src);
        return -1;
    }

    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        snprintf(g_err, sizeof g_err, "cannot write %s", dst);
        return -1;
    }

    memset(&s, 0, sizeof s);
    /* workFactor 0 means the default (30). */
    rc = BZ2_bzCompressInit(&s, BZ_BLOCK_100K, 0, 0);
    if (rc != BZ_OK) {
        seterr("starting compression", rc);
        goto fail;
    }
    started = 1;

    for (;;) {
        size_t n = fread(inbuf, 1, sizeof inbuf, in);

        if (n == 0)
            break;

        s.next_in = inbuf;
        s.avail_in = (unsigned int)n;

        while (s.avail_in > 0) {
            s.next_out = outbuf;
            s.avail_out = sizeof outbuf;

            rc = BZ2_bzCompress(&s, BZ_RUN);
            if (rc != BZ_RUN_OK) {
                seterr("compressing", rc);
                goto fail;
            }

            {
                size_t have = sizeof outbuf - s.avail_out;

                if (have && fwrite(outbuf, 1, have, out) != have) {
                    snprintf(g_err, sizeof g_err, "write error on %s", dst);
                    goto fail;
                }
            }
        }
    }

    if (ferror(in)) {
        snprintf(g_err, sizeof g_err, "read error on %s", src);
        goto fail;
    }

    /* Flush. BZ_FINISH_OK means there is more to come; BZ_STREAM_END means
       that was the last of it. */
    for (;;) {
        size_t have;

        s.next_out = outbuf;
        s.avail_out = sizeof outbuf;

        rc = BZ2_bzCompress(&s, BZ_FINISH);
        if (rc != BZ_FINISH_OK && rc != BZ_STREAM_END) {
            seterr("finishing compression", rc);
            goto fail;
        }

        have = sizeof outbuf - s.avail_out;
        if (have && fwrite(outbuf, 1, have, out) != have) {
            snprintf(g_err, sizeof g_err, "write error on %s", dst);
            goto fail;
        }

        if (rc == BZ_STREAM_END)
            break;
    }

    BZ2_bzCompressEnd(&s);
    fclose(in);

    if (fclose(out) != 0) {
        snprintf(g_err, sizeof g_err, "could not finish writing %s", dst);
        remove(dst);
        return -1;
    }
    return 0;

fail:
    if (started)
        BZ2_bzCompressEnd(&s);
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    /* A half-written .bz2 is worse than none: it would be served to a peer
       as if it were a file list. */
    remove(dst);
    return -1;
}

/* ----------------------------------------------------------- decompress */

struct bz_reader {
    FILE      *fp;
    bz_stream  s;
    char       in[BZ_CHUNK];
    int        started;
    int        done;        /* stream ended, or failed */
    int        file_eof;
};

bz_reader *bz_open(const char *path)
{
    bz_reader *r;
    int small;
    int rc;

    g_err[0] = '\0';

    r = (bz_reader *)calloc(1, sizeof *r);
    if (!r) {
        snprintf(g_err, sizeof g_err, "out of memory");
        return NULL;
    }

    r->fp = fopen(path, "rb");
    if (!r->fp) {
        snprintf(g_err, sizeof g_err, "cannot read %s", path);
        free(r);
        return NULL;
    }

    /* `small` halves the decompressor's appetite -- 2.5MB rather than up to
       7.6MB for a stream compressed at block size 9 -- at roughly half the
       speed. On DOS that trade is worth making: a file list from a stranger
       may have been compressed with any block size, and a client that dies
       of memory exhaustion because someone else chose -9 is indefensible.
       On the host there is no reason to be slow about it. */
#ifdef DOSYPP_WATT32
    small = 1;
#else
    small = 0;
#endif

    rc = BZ2_bzDecompressInit(&r->s, 0, small);
    if (rc != BZ_OK) {
        seterr("starting decompression", rc);
        fclose(r->fp);
        free(r);
        return NULL;
    }
    r->started = 1;
    return r;
}

int bz_read(bz_reader *r, void *buf, size_t len)
{
    if (!r || r->done)
        return 0;
    if (len == 0)
        return 0;
    if (len > 0x7FFF0000u)
        len = 0x7FFF0000u;

    r->s.next_out = (char *)buf;
    r->s.avail_out = (unsigned int)len;

    for (;;) {
        int rc;

        if (r->s.avail_in == 0 && !r->file_eof) {
            size_t n = fread(r->in, 1, sizeof r->in, r->fp);

            if (n == 0) {
                r->file_eof = 1;
            } else {
                r->s.next_in = r->in;
                r->s.avail_in = (unsigned int)n;
            }
        }

        if (r->s.avail_in == 0 && r->file_eof) {
            /* The file ran out before the stream said it was finished. A
               file list that stops mid-stream is what a peer disconnecting
               looks like, and it must not read as a complete list. */
            seterr("decompressing", BZ_UNEXPECTED_EOF);
            r->done = 1;
            return -1;
        }

        rc = BZ2_bzDecompress(&r->s);

        if (rc == BZ_STREAM_END) {
            /* Whatever came out on this call is still good data. A second
               concatenated stream (bzip2 permits them) is deliberately not
               followed: no client makes one for a file list, and reading on
               would mean trusting a stranger's framing further than
               necessary. */
            r->done = 1;
            return (int)(len - r->s.avail_out);
        }

        if (rc != BZ_OK) {
            seterr("decompressing", rc);
            r->done = 1;
            return -1;
        }

        if (r->s.avail_out == 0)
            return (int)len;        /* caller's buffer is full */

        /* Otherwise: more input needed, round again. */
    }
}

void bz_close(bz_reader **rp)
{
    bz_reader *r = rp ? *rp : NULL;

    if (!r)
        return;

    if (r->started)
        BZ2_bzDecompressEnd(&r->s);
    if (r->fp)
        fclose(r->fp);
    free(r);
    *rp = NULL;
}
