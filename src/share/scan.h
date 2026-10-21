/* scan.h - walking the share roots.
 *
 * Incremental on purpose: scan_step() visits a bounded number of entries
 * and returns, so a share on a slow disk does not freeze the UI or stop the
 * hub connection being serviced. A 10,000-file tree on a 486 with period
 * IDE takes a noticeable while just to enumerate.
 *
 * The directory walk is the one part of the share subsystem that genuinely
 * differs by platform: FINDFIRST/FINDNEXT on DOS, opendir/readdir on the
 * host.
 */
#ifndef DOSYPP_SHARE_SCAN_H
#define DOSYPP_SHARE_SCAN_H

#include "index.h"

#define SCAN_PATH_MAX  200
/* An escaped path: every byte could become "\s", so twice the length. */
#define SCAN_ESC_MAX   (SCAN_PATH_MAX * 2 + 4)
#define SCAN_STACK_MAX 16     /* directory nesting; deeper is skipped */

typedef struct scan_state scan_state;

/* Begin scanning the index's configured roots. Clears the index first.
   Returns 0, or -1 if there is nothing to scan. */
int scan_begin(scan_state **ss, share_index *ix);

/* Visit up to `budget` directory entries. Returns:
     1  more to do
     0  finished (and *ss is freed and set to NULL)
    -1  gave up; see scan_error()  */
int scan_step(scan_state **ss, int budget);

void scan_cancel(scan_state **ss);

const char *scan_error(const scan_state *ss);

/* What is being looked at right now, for the progress display. */
const char *scan_current(const scan_state *ss);
unsigned int scan_dirs_done(const scan_state *ss);

/* Skip rules, applied to every entry. Exposed because they are a policy
   decision worth testing: hidden files, anything that looks like a
   partial download, and our own state files must not be shared. */
int scan_should_skip(const char *name);

#endif /* DOSYPP_SHARE_SCAN_H */
