/* browse.h - somebody else's share, as a thing you can walk around in.
 *
 * A fetched file list is parsed once into a flat table of entries, each
 * pointing at its parent directory. Navigation is then just a filter on
 * that table -- no reparsing, no seeking, and no tree of pointers to get
 * wrong.
 *
 * Bounded on purpose. A hub regular with 200,000 files has a list this
 * machine cannot hold in any representation, so there is a cap, and when it
 * is hit the list is marked truncated and the door says so. Silently
 * showing part of someone's share as though it were all of it is the one
 * outcome worth ruling out.
 *
 * Sizes, in DOS terms: an entry is 24 bytes plus its name in a shared pool,
 * so the 12,000-entry cap costs about 288KB of table and perhaps 250KB of
 * names. That fits beside the share index and the bzip2 decompressor on a
 * 16MB machine, which is what sets the cap.
 */
#ifndef DOSYPP_SHARE_BROWSE_H
#define DOSYPP_SHARE_BROWSE_H

#include "../hash/tth.h"
#include "xmlpull.h"

#define BROWSE_MAX_ENTRIES 12000
#define BROWSE_NO_PARENT   (-1)

typedef struct {
    unsigned long name_off;    /* in the pool */
    long          parent;      /* index of the containing directory */
    tth_u64       size;        /* 0 for a directory */
    unsigned char tth[TTH_HASH_LEN];
    unsigned char is_dir;
    unsigned char have_tth;

    /* Partial lists. A directory the peer mentioned but did not describe
       is `incomplete`; one whose contents have arrived is `loaded`. A
       directory that is neither is an empty one, and the door shows it as
       empty rather than as something still to fetch. */
    unsigned char incomplete;
    unsigned char loaded;
} browse_entry;

typedef struct {
    browse_entry *ents;
    long          count;
    long          cap;

    char         *pool;
    unsigned long pool_len;
    unsigned long pool_cap;

    long          cwd;         /* current directory, or BROWSE_NO_PARENT */
    int           truncated;   /* the list was bigger than the cap */

    /* Whose list this is, for the door's title. */
    char          nick[32];

    /* The Base the last list declared, which for a partial list is the
       directory it describes. Checked rather than trusted: a peer
       answering with a listing of somewhere else is not answering. */
    char          base[128];

    /* Set when the whole share arrived in one list. Then nothing is ever
       incomplete and no directory needs fetching. */
    int           whole;

    char          err[160];
} browse_list;

void browse_init(browse_list *b);
void browse_free(browse_list *b);

/* Parse a .bz2 file list. Returns the number of entries, or -1 with the
   reason in browse_error(). A truncated list is not an error -- a partial
   view of a big share is useful, as long as it says so. */
long browse_load_bz2(browse_list *b, const char *path, const char *nick);

/* Parse from an already-open pull source. Exposed for testing with plain
   XML, which is how the parser is exercised without a compressor. */
long browse_load(browse_list *b, xp_fill fill, void *user,
                 const char *nick);

const char *browse_error(const browse_list *b);

/* Merge a partial list into an existing tree, as the contents of
   `parent`.
 *
 * Returns the number of entries added, or -1 with the reason in
 * browse_error(). A directory whose contents are already present is left
 * alone and 0 is returned: the entries are indices and other entries point
 * at them, so replacing a subtree would mean renumbering the table. Within
 * one session a share does not change underneath us, so there is nothing
 * to gain by it.
 *
 * The list's own Base is checked against `expect_base` when that is given:
 * a peer that answers a request for /a/ with a listing of /b/ would
 * otherwise have its files filed under the wrong directory. */
long browse_merge_bz2(browse_list *b, const char *path, long parent,
                      const char *expect_base);

/* Does entering this directory mean asking the peer for it? True for a
   directory from a partial list whose contents have not arrived. */
int browse_needs_fetch(const browse_list *b, long index);

/* An entry's full virtual path, with a trailing slash for a directory:
   "/warez/apps/". This is what goes in a CGET. */
void browse_entry_path(const browse_list *b, long index, char *out,
                       size_t outsz);

/* Navigation. browse_enter() descends into an entry (a no-op for a file);
   browse_up() goes to the parent. */
void browse_enter(browse_list *b, long index);
void browse_up(browse_list *b);

/* The entries directly inside the current directory, as indices into the
   table, directories first and then files, each group in list order.
   Returns how many were written. */
long browse_listing(browse_list *b, long *out, long max);

/* The current directory as a path, for the door's header. */
void browse_path(const browse_list *b, char *out, size_t outsz);

const char *browse_name(const browse_list *b, long index);

/* Base32 of an entry's hash, for queueing a download. Returns 0 on
   success, -1 if the entry has no usable hash (every directory, and any
   file whose list entry omitted one). */
int browse_tth(const browse_list *b, long index, char *out, size_t outsz);

#endif /* DOSYPP_SHARE_BROWSE_H */
