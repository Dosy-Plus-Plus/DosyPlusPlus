/* tokenize.h - ADC line framing, escaping, and message parsing.
 *
 * Pure C, no DOS and no socket dependency, so it unit-tests natively. Every
 * ADC message is:
 *
 *     <type><CMD> <param> <param> ... \n
 *
 * type is one of HIBCDEFU, CMD is three uppercase chars, params are
 * space-separated and escaped: "\s" = space, "\n" = newline, "\\" = backslash.
 * Named fields are a two-char key followed by the (escaped) value.
 */
#ifndef DOSYPP_TOKENIZE_H
#define DOSYPP_TOKENIZE_H

#include <stddef.h>

#include "../hash/tiger.h"   /* tiger_u64: sizes in ADC are 64-bit */
#include "../common.h"

#define ADC_MAX_PARAMS 48

typedef struct {
    char  type;                      /* 'H','I','B','C','D','E','F','U' */
    char  cmd[4];                    /* three chars + NUL */
    int   nparam;
    char *param[ADC_MAX_PARAMS];     /* unescaped, point into store[] */
    char  store[ADC_LINE_MAX];
} adc_msg;

/* --- escaping --- */

/* Unescape src into dst (at most dstsz bytes including NUL).
   Returns the unescaped length, or -1 if it would not fit. */
int adc_unescape(char *dst, size_t dstsz, const char *src);

/* Escape src into dst. Returns escaped length, or -1 if it would not fit. */
int adc_escape(char *dst, size_t dstsz, const char *src);

/* A 64-bit value as decimal, because "%lu" is 32 bits on the target.
 *
 * Sizes in ADC are 64-bit and a share is easily bigger than 4GB -- ten
 * thousand files of a megabyte each is ten. Printing one through
 * (unsigned long) on DOS advertises the remainder modulo 4GB, which is how
 * a 5GB share tells a hub it has 700MB. `out` needs 21 bytes. */
void adc_u64_dec(tiger_u64 v, char *out, size_t outsz);

/* Decimal to 64-bit, saturating rather than wrapping. A field from the
   network is not a promise: thirty digits of nines must come back as
   "enormous", not as whatever that happens to be modulo 2^64. */
tiger_u64 adc_u64_parse(const char *s);

/* --- message parsing --- */

/* Parse one complete message (without its trailing newline).
   Returns 0 on success, -1 if malformed. Empty lines are a keepalive and
   parse to type 0 with nparam 0, which callers should ignore rather than
   treat as an error. */
int adc_parse(adc_msg *m, const char *line);

/* Value of a named field, e.g. adc_field(m, "NI") for a nick. Returns NULL if
   absent. Leading positional header parameters (SIDs, CIDs, feature lists)
   are skipped automatically based on message type. */
const char *adc_field(const adc_msg *m, const char *key);

/* How many leading positional parameters the type prescribes:
     B <sid>                     -> 1
     D <sid> <target>            -> 2
     E <sid> <target>            -> 2
     F <sid> <+feat-feat>        -> 2
     U <cid>                     -> 1
     H, I, C                     -> 0
   Anything else returns 0. */
int adc_hdr_params(char type);

/* Non-zero if the message carries a leading SID parameter. */
int adc_has_sid(char type);

/* --- line framing --- */

/* Accumulates bytes off the wire and hands back whole lines. A partial line
   stays here until the rest arrives. */
typedef struct {
    char   buf[ADC_LINE_MAX];
    size_t len;
    int    overflow;   /* set once a line exceeded ADC_LINE_MAX */
} linebuf;

void linebuf_init(linebuf *lb);

/* Append raw bytes. Returns 0, or -1 if the line is over-long (the buffer is
   then dropped and resynchronised at the next newline). */
int linebuf_feed(linebuf *lb, const char *data, size_t len);

/* Pull the next complete line, NUL-terminated and without the newline.
   Returns 1 and fills out[] on success, 0 when no complete line is buffered.
   out must be at least ADC_LINE_MAX bytes. */
int linebuf_next(linebuf *lb, char *out);

/* Whatever is buffered but not yet consumed as a line.
 *
 * This exists for one specific and unavoidable reason: a peer sends a
 * "CSND ... <len>" line immediately followed by <len> raw bytes, usually in
 * the same TCP segment. By the time the CSND line has been parsed, the
 * first chunk of the file is already sitting in this buffer. Without a way
 * to take it back out, those bytes are lost and every transfer corrupts.
 *
 * Returns the number of bytes available and sets *data to them. */
size_t linebuf_pending(const linebuf *lb, const char **data);

/* Discard the first n pending bytes, after copying them elsewhere. */
void linebuf_consume(linebuf *lb, size_t n);

#endif /* DOSYPP_TOKENIZE_H */
