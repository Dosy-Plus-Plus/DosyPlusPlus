/* xmlpull.h - just enough XML to read a file list.
 *
 * A files.xml is a tree of two element types:
 *
 *   <FileListing Version="1" CID="..." Base="/" Generator="...">
 *     <Directory Name="warez">
 *       <File Name="DOOM19S.ZIP" Size="180000" TTH="ZMOK...4V4Q"/>
 *     </Directory>
 *   </FileListing>
 *
 * This is a pull parser: the caller asks for the next event and gets one
 * item at a time, so a 20MB list from someone with 200,000 files is never
 * held in memory. Bytes come from a callback, which is how the same parser
 * reads a plain file in tests and a bzip2 stream in the client.
 *
 * It is NOT a general XML parser and does not pretend to be. No DTDs, no
 * namespaces, no mixed content, no CDATA. Text between elements is
 * discarded, because a file list has none that means anything.
 *
 * Everything here assumes the input is hostile. A file list arrives from a
 * stranger over a socket, so:
 *
 *   - Every string is bounded and truncated rather than overflowed.
 *   - Nesting is capped; a list of ten thousand nested directories is a
 *     denial of service, not a share.
 *   - Unknown elements are skipped but still counted, so the tree depth
 *     stays right and a stray </Directory> cannot unbalance it.
 *   - Truncation anywhere is an error, never a silent end of list.
 *
 * Names come out as UTF-8, exactly as the file list carries them. The caller
 * transliterates for display (ui/cp437.c); downloads are addressed by hash,
 * so a name that does not survive the trip to CP437 costs nothing but
 * legibility.
 */
#ifndef DOSYPP_SHARE_XMLPULL_H
#define DOSYPP_SHARE_XMLPULL_H

#include <stddef.h>

#include "../hash/tth.h"

#define XP_NAME_MAX  128
#define XP_DEPTH_MAX 32

typedef enum {
    XP_END = 0,        /* clean end of document */
    XP_DIR_OPEN,       /* entering a <Directory>; name is set */
    XP_DIR_CLOSE,      /* leaving one */
    XP_FILE,           /* a <File>; name, size and tth are set */
    XP_OTHER,          /* any other element, when `want_other` is set:
                          elem is its name and xp_attr reads its
                          attributes. A hublist is <Hub .../> elements, so
                          this is how one file format serves two. */
    XP_ERROR           /* malformed or truncated; xp_error() says why */
} xp_event;

/* Attributes kept for XP_OTHER. Bounded, like everything else here: a
   hublist entry has a dozen and a hostile one could claim thousands. */
#define XP_MAX_ATTRS   12
#define XP_ATTR_KEY    24
#define XP_ATTR_VAL    96

/* Fill `buf` with up to `len` bytes. Returns the count, 0 at the end, or
   -1 on error -- the same contract as bz_read, which is what the client
   passes in. */
typedef int (*xp_fill)(void *user, void *buf, size_t len);

typedef struct {
    xp_fill  fill;
    void    *user;

    char     buf[1024];
    size_t   len;
    size_t   pos;
    int      eof;

    int      depth;            /* element nesting, including unknowns */
    int      pending_close;    /* a self-closed <Directory/> owes a close */
    int      done;

    /* Set by the caller: report elements other than Directory and File
       as XP_OTHER rather than skipping them. */
    int      want_other;

    /* The current item. */
    char     elem[48];         /* the element's name */
    char     name[XP_NAME_MAX];
    tth_u64  size;
    char     tth[BASE32_TIGER_LEN + 1];
    int      have_tth;

    /* A directory in a partial list whose contents were not sent:
       DC++ marks those Incomplete="1". Set per XP_DIR_OPEN. Browsing one
       means asking for it. */
    int      incomplete;

    /* The Base attribute of <FileListing>, which says which directory a
       partial list is of. Retained for the whole document, because it is
       read once at the top and needed when the entries arrive. "/" for a
       full list. */
    char     base[XP_NAME_MAX];

    /* Generic attributes, for XP_OTHER. */
    struct {
        char key[XP_ATTR_KEY];
        char val[XP_ATTR_VAL];
    }        attr[XP_MAX_ATTRS];
    int      nattr;

    char     err[128];
} xmlpull;

void xp_init(xmlpull *x, xp_fill fill, void *user);

/* The next event. Returns XP_END at the end of the document and keeps
   returning it; XP_ERROR likewise, so a caller that ignores the return
   value loops rather than reading rubbish. */
xp_event xp_next(xmlpull *x);

const char *xp_error(const xmlpull *x);

/* An attribute of the current XP_OTHER element, or NULL. Case sensitive,
   as XML is. */
const char *xp_attr(const xmlpull *x, const char *key);

/* Decode XML entity references in place (&amp; &lt; &gt; &quot; &apos; and
   numeric forms). Exposed for testing: a file list full of &amp; in names
   is normal, and getting this wrong corrupts every name after the first. */
void xp_unescape(char *s);

#endif /* DOSYPP_SHARE_XMLPULL_H */
