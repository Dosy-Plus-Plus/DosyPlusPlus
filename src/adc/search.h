/* search.h - ADC search, and the results that come back.
 *
 * Outbound:  BSCH <sid> TO<token> AN<term> ... | TR<tth>
 * Inbound:   DRES <from> <sid> FN<path> SI<size> SL<slots> TR<tth>   (TCP)
 *            URES <cid> FN... SI... SL... TR...                      (UDP)
 *
 * Two things shape this more than the protocol does:
 *
 *   - Results are addressed by TTH, so the same file offered by thirty
 *     people is thirty results for one file. They are coalesced by TTH into
 *     a single row carrying a source list, which is both how a download
 *     gets multiple sources later and the difference between a usable list
 *     and thirty screens of the same name.
 *
 *   - Memory is finite. A popular search on a busy hub can return thousands
 *     of results; this keeps a bounded table and drops the worst once it is
 *     full, rather than growing until a 16MB machine dies.
 *
 * Tokens matter: a result carries the token of the search it answers, and
 * results for a search you have moved on from must be discarded rather than
 * mixed into the current list.
 *
 * No DOS dependency; unit tested.
 */
#ifndef DOSYPP_SEARCH_H
#define DOSYPP_SEARCH_H

#include "ident.h"
#include "tokenize.h"
#include "userlist.h"   /* ADC_NICK_MAX */
#include "../hash/tth.h"

#define SR_NAME_MAX    64     /* display name, after the path is trimmed */
#define SR_PATH_MAX    128    /* the FN value we must send back to ask */
#define SR_MAX_RESULTS 192    /* bounded on purpose; see above */
#define SR_MAX_SOURCES 12     /* per result */

typedef enum {
    SEARCH_ANY = 0,
    SEARCH_AUDIO,
    SEARCH_ARCHIVE,
    SEARCH_DOCUMENT,
    SEARCH_EXECUTABLE,
    SEARCH_PICTURE,
    SEARCH_VIDEO,
    SEARCH_FOLDER,
    SEARCH_TTH,            /* exact, by hash */
    SEARCH_GROUP_COUNT
} search_group;

typedef struct {
    char          terms[96];        /* space separated AN terms */
    char          exclude[64];      /* NO terms */
    char          tth[BASE32_TIGER_LEN + 1];
    search_group  group;
    tiger_u64     min_size;
    tiger_u64     max_size;
} search_query;

/* A source is just a SID and a slot count.
 *
 * It deliberately does NOT carry the peer's path or nick:
 *
 *   - The path is unnecessary. ADC downloads are addressed by hash --
 *     CGET file TTH/<root> -- so once a result has a TTH, any source will
 *     serve it without us remembering their particular path. Only
 *     directories, which have no TTH, need one, and the result keeps a
 *     single path for that.
 *   - The nick is looked up from the user list by SID at draw time. Copying
 *     it here would duplicate data that changes when someone renames.
 *
 * This is not micro-optimisation. Storing a 128-byte path per source made
 * search_state 756KB, which put a 760KB hubconn on the stack: over DJGPP's
 * 512KB default, so it would have crashed on DOS without fail. It crashed
 * on the host first. */
typedef struct {
    adc_sid   sid;
    unsigned  slots;
} search_source;

typedef struct {
    char          name[SR_NAME_MAX];        /* basename, for display */
    char          path[SR_PATH_MAX];        /* the FN we would ask for */
    char          tth[BASE32_TIGER_LEN + 1];
    tiger_u64     size;
    int           is_dir;

    search_source sources[SR_MAX_SOURCES];
    int           nsource;
    int           dropped_sources;          /* beyond SR_MAX_SOURCES */

    int           tagged;                   /* space-bar tagging */
} search_result;

typedef struct {
    search_result results[SR_MAX_RESULTS];
    int           count;
    int           dropped;        /* results discarded, table full */

    char          token[16];      /* the search these belong to */
    unsigned long token_seq;
    int           active;         /* a search is outstanding */
    unsigned long started;        /* seconds, for the "searching..." state */

    search_query  query;          /* what was asked, for redisplay */
} search_state;

void search_init(search_state *ss);

/* Start a new search: clears the previous results and mints a fresh token.
   Writes the BSCH line into `out`. Returns 0, or -1 if the query is empty
   or would not fit. `now` is seconds. */
int search_build(search_state *ss, adc_sid own_sid, const search_query *q,
                 char *out, size_t outsz, unsigned long now);

/* Feed a DRES or URES message. Returns 1 if it was accepted, 0 if it was
   ignored (wrong token, malformed, table full). */
int search_add_result(search_state *ss, const adc_msg *m);

/* A compile-time guard: this struct lives inside hubconn, which is a local
   in main(), and DJGPP's default stack is 512KB. If a future field pushes
   it over this, the DOS build must fail here rather than at run time on
   someone's 486. */
typedef char search_state_is_not_absurd[
    (sizeof(search_result) * SR_MAX_RESULTS < 128 * 1024) ? 1 : -1];

/* Sorting, because a results list that cannot be sorted by size is not much
   use for finding the good copy. */
typedef enum {
    SORT_SOURCES = 0,   /* most sources first: the one most likely to work */
    SORT_NAME,
    SORT_SIZE,
    SORT_COUNT
} search_sort;

void search_sort_by(search_state *ss, search_sort how);
const char *search_sort_name(search_sort how);

/* Tagging. */
void search_tag_toggle(search_state *ss, int index);
void search_tag_all(search_state *ss, int on);
int  search_tagged_count(const search_state *ss);

const char *search_group_name(search_group g);

/* Human-readable size, no floating point. */
void search_fmt_size(tiger_u64 bytes, char *out, size_t outsz);

#endif /* DOSYPP_SEARCH_H */
