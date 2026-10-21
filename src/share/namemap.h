/* namemap.h - long filenames on a filesystem that has none.
 *
 * ADC filenames are UTF-8 and arbitrarily long. DOS gives you 8.3. This is
 * the single most DOS-specific problem in the whole project, and it is a
 * mapping problem, not an escaping one: there is no reversible way to
 * squeeze "Some Long Archive Name (1994).zip" into eleven characters.
 *
 * So: a downloaded file is stored under a generated 8.3 name, and a
 * persistent map records what it is really called. The UI always shows the
 * real name; only the filesystem ever sees the short one.
 *
 * The map lives in DOSYPP.NAM. Losing it must be survivable -- the files
 * are still there and still verifiable by hash -- so it is written
 * defensively and a corrupt line is skipped rather than fatal. It is not,
 * however, a cache: lose it and you lose the names, which is why it is
 * written as soon as a name is assigned rather than at exit.
 *
 * Short names are derived from the real one where possible, because
 * "DOOM19S.ZIP" is worth more to someone looking at a DOS directory
 * listing than "D0000017.DAT". A collision falls back to a counter.
 */
#ifndef DOSYPP_NAMEMAP_H
#define DOSYPP_NAMEMAP_H

#include <stddef.h>

#define NAMEMAP_FILE   "DOSYPP.NAM"
#define NAMEMAP_SHORT  13      /* 8 + '.' + 3 + NUL */
#define NAMEMAP_LONG   128
#define NAMEMAP_MAX    512     /* entries held in memory */

typedef struct namemap namemap;

int  namemap_init(namemap **nm);
void namemap_free(namemap **nm);

/* Load the map. A missing file is not an error. Returns the number of
   entries read. */
int  namemap_load(namemap *nm, const char *path);

/* Append one entry to the file. Done per assignment rather than at exit:
   a crash between downloading a file and recording its name would leave a
   file nobody can identify. */
int  namemap_append(namemap *nm, const char *path, const char *shortname,
                    const char *longname);

/* Assign a short name for `longname`, avoiding anything already mapped and
   anything `exists` reports as present on disk. Writes NAMEMAP_SHORT bytes.
   `exists` may be NULL, in which case only the map is consulted.
   Returns 0, or -1 if no name could be found. */
int  namemap_assign(namemap *nm, const char *longname, char *out,
                    int (*exists)(const char *name, void *user),
                    void *user);

/* Look up either direction. Returns NULL if unknown. */
const char *namemap_long(const namemap *nm, const char *shortname);
const char *namemap_short(const namemap *nm, const char *longname);

/* Reduce a long name to a plausible 8.3 candidate: upper-cased, illegal
   characters dropped, extension preserved. Exposed because it is the part
   worth testing directly. */
void namemap_squash(const char *longname, char *out);

#endif /* DOSYPP_NAMEMAP_H */
