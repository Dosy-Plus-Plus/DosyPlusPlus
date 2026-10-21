/* scan.c - incremental recursive directory walk. */
#include "scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOSYPP_WATT32
#  define PATH_SEP '\\'
#else
#  define PATH_SEP '/'
#endif

/* Case-insensitive equality. DOS filenames are case-insensitive, and a
   share must not depend on how the filesystem happened to spell them. */
static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* One level of the walk. An explicit stack rather than recursion: a deep
   tree must not cost stack we do not have on DOS, and an incremental walk
   has to be able to stop and resume between calls anyway. */
typedef struct {
    void *handle;                   /* DIR* or a DOS find block */
    char  local[SCAN_PATH_MAX];     /* local path of this directory */
    char  virt[SCAN_PATH_MAX];      /* advertised path of this directory */
    int   open;
} scan_level;

struct scan_state {
    share_index *ix;
    scan_level   stack[SCAN_STACK_MAX];
    int          sp;
    int          root;              /* next root to start */
    unsigned int dirs;
    char         current[SCAN_PATH_MAX];
    char         err[128];
};

int scan_should_skip(const char *name)
{
    size_t n;

    if (!name || !*name)
        return 1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 1;

    /* Hidden files, by the Unix convention. On DOS the attribute is checked
       as well. */
    if (name[0] == '.')
        return 1;

    /* Our own files have no business in a share, and they all end up in
       one: the program is usually run from the directory it shares, so
       every file it writes lands there. This list was three names long
       until a browse test showed PARTLIST.BZ2 and DOSYPP.TTL being offered
       to a peer -- which is not a leak of anything secret, but it is a
       client advertising its own droppings as warez.

       Matched by name rather than by a wildcard so that somebody's
       DOSYPP.ZIP is still shared. */
    {
        static const char *const OURS[] = {
            "DOSYPP.INI", "DOSYPP.TTH", "DOSYPP.NAM", "DOSYPP.QUE",
            "DOSYPP.FAV", "DOSYPP.TTL", "DOSYPP.WAV",
            "FILES.XML", "FILES.BZ2",
            "PARTLIST.XML", "PARTLIST.BZ2", "PEERLIST.BZ2",
            "TTHL.TMP"
        };
        size_t k;

        for (k = 0; k < sizeof OURS / sizeof OURS[0]; k++)
            if (ci_eq(name, OURS[k]))
                return 1;
    }

    /* A partial download is not a file anyone can use, and offering one
       produces a transfer that fails verification. */
    n = strlen(name);
    if (n > 5 && ci_eq(name + n - 5, ".PART"))
        return 1;
    if (n > 4 && ci_eq(name + n - 4, ".TMP"))
        return 1;

    return 0;
}

static const char *base_of(const char *path)
{
    const char *last = path;

    for (; *path; path++)
        if (*path == '/' || *path == '\\')
            last = path + 1;
    return last;
}

static void join(char *out, size_t outsz, const char *dir, const char *name,
                 char sep)
{
    size_t n = strlen(dir);

    if (n == 0) {
        snprintf(out, outsz, "%s", name);
        return;
    }
    if (dir[n - 1] == '/' || dir[n - 1] == '\\')
        snprintf(out, outsz, "%s%s", dir, name);
    else
        snprintf(out, outsz, "%s%c%s", dir, sep, name);
}

/* --- platform layer -------------------------------------------------- */

typedef struct {
    char      name[SCAN_PATH_MAX];
    int       is_dir;
    tth_u64   size;
    unsigned long mtime;
} scan_entry;

static int level_open(scan_level *lv);
static int level_next(scan_level *lv, scan_entry *e);
static void level_close(scan_level *lv);

/* --- the walk -------------------------------------------------------- */

static int push_dir(scan_state *s, const char *local, const char *virt)
{
    scan_level *lv;

    if (s->sp >= SCAN_STACK_MAX)
        return 0;                   /* too deep: skip rather than fail */

    lv = &s->stack[s->sp];
    memset(lv, 0, sizeof *lv);
    snprintf(lv->local, sizeof lv->local, "%s", local);
    snprintf(lv->virt, sizeof lv->virt, "%s", virt);

    if (level_open(lv) != 0)
        return 0;                   /* unreadable: skip it, keep going */

    s->sp++;
    s->dirs++;
    return 1;
}

int scan_begin(scan_state **ssp, share_index *ix)
{
    scan_state *s;

    *ssp = NULL;
    if (!ix || ix->nroot == 0)
        return -1;

    s = (scan_state *)calloc(1, sizeof *s);
    if (!s)
        return -1;

    s->ix = ix;
    share_clear(ix);
    *ssp = s;
    return 0;
}

void scan_cancel(scan_state **ssp)
{
    scan_state *s = *ssp;

    if (!s)
        return;
    while (s->sp > 0)
        level_close(&s->stack[--s->sp]);
    free(s);
    *ssp = NULL;
}

const char *scan_error(const scan_state *ss)
{
    return (ss && ss->err[0]) ? ss->err : "no error";
}

const char *scan_current(const scan_state *ss)
{
    return ss ? ss->current : "";
}

unsigned int scan_dirs_done(const scan_state *ss)
{
    return ss ? ss->dirs : 0;
}

int scan_step(scan_state **ssp, int budget)
{
    scan_state *s = *ssp;
    int seen = 0;

    if (!s)
        return 0;

    while (seen < budget) {
        scan_entry e;
        scan_level *lv;

        /* Nothing open: start the next root. */
        if (s->sp == 0) {
            if (s->root >= s->ix->nroot) {
                scan_cancel(ssp);
                return 0;           /* done */
            }
            {
                const char *r = s->ix->roots[s->root++];
                /* The advertised path begins with the root's own last
                   component, so a share of C:\DL appears as "DL/..." rather
                   than leaking the local layout. */
                push_dir(s, r, base_of(r));
            }
            continue;
        }

        lv = &s->stack[s->sp - 1];

        if (level_next(lv, &e) != 0) {
            level_close(lv);
            s->sp--;
            continue;
        }

        seen++;

        if (scan_should_skip(e.name))
            continue;

        if (e.is_dir) {
            char local[SCAN_PATH_MAX], virt[SCAN_PATH_MAX];

            join(local, sizeof local, lv->local, e.name, PATH_SEP);
            join(virt, sizeof virt, lv->virt, e.name, '/');
            snprintf(s->current, sizeof s->current, "%s", virt);
            push_dir(s, local, virt);
            continue;
        }

        /* Zero-length files are legal but pointless to share, and they
           make for a confusing transfer. */
        if (e.size == 0)
            continue;

        {
            char local[SCAN_PATH_MAX], virt[SCAN_PATH_MAX];

            join(local, sizeof local, lv->local, e.name, PATH_SEP);
            join(virt, sizeof virt, lv->virt, e.name, '/');
            snprintf(s->current, sizeof s->current, "%s", virt);

            if (!share_add_file(s->ix, virt, local, e.size, e.mtime)) {
                snprintf(s->err, sizeof s->err,
                         "out of memory after %u files", s->ix->count);
                return -1;
            }
        }
    }

    return 1;                       /* more to do */
}

/* --- platform layer: DOS -------------------------------------------- */

#ifdef DOSYPP_WATT32

#include <dir.h>
#include <dos.h>

/* FINDFIRST/FINDNEXT keep their state in the caller's ffblk, so one block
   per open directory level. */
typedef struct {
    struct ffblk blk;
    int          first;
} dos_find;

static int level_open(scan_level *lv)
{
    dos_find *df = (dos_find *)calloc(1, sizeof *df);
    char pattern[SCAN_PATH_MAX];
    size_t n;

    if (!df)
        return -1;

    n = strlen(lv->local);
    if (n + 5 >= sizeof pattern) {
        free(df);
        return -1;
    }

    /* "DIR\*.*" -- DOS needs an explicit wildcard, and a trailing
       separator must not be doubled. */
    if (n > 0 && (lv->local[n - 1] == '\\' || lv->local[n - 1] == '/'))
        snprintf(pattern, sizeof pattern, "%s*.*", lv->local);
    else
        snprintf(pattern, sizeof pattern, "%s\\*.*", lv->local);

    /* FA_DIREC so subdirectories are returned; hidden and system entries
       are deliberately not requested. */
    if (findfirst(pattern, &df->blk, FA_DIREC | FA_RDONLY | FA_ARCH) != 0) {
        free(df);
        return -1;                  /* empty or unreadable */
    }

    df->first = 1;
    lv->handle = df;
    lv->open = 1;
    return 0;
}

static int level_next(scan_level *lv, scan_entry *e)
{
    dos_find *df = (dos_find *)lv->handle;

    if (!df)
        return -1;

    if (!df->first) {
        if (findnext(&df->blk) != 0)
            return -1;
    }
    df->first = 0;

    snprintf(e->name, sizeof e->name, "%s", df->blk.ff_name);
    e->is_dir = (df->blk.ff_attrib & FA_DIREC) ? 1 : 0;
    e->size = (tth_u64)df->blk.ff_fsize;

    /* ff_fdate/ff_ftime are packed DOS date and time. Combined into one
       comparable number: the hash cache only needs "did this change", not a
       real timestamp. */
    e->mtime = ((unsigned long)df->blk.ff_fdate << 16) |
               (unsigned long)df->blk.ff_ftime;

    /* Hidden and system files are skipped even if the mask let them in. */
    if (df->blk.ff_attrib & (FA_HIDDEN | FA_SYSTEM))
        e->name[0] = '.';           /* makes scan_should_skip reject it */

    return 0;
}

static void level_close(scan_level *lv)
{
    free(lv->handle);
    lv->handle = NULL;
    lv->open = 0;
}

#else

/* --- platform layer: POSIX host -------------------------------------- */

#include <dirent.h>
#include <sys/stat.h>

static int level_open(scan_level *lv)
{
    DIR *d = opendir(lv->local);

    if (!d)
        return -1;
    lv->handle = d;
    lv->open = 1;
    return 0;
}

static int level_next(scan_level *lv, scan_entry *e)
{
    DIR *d = (DIR *)lv->handle;
    struct dirent *de;
    struct stat st;
    char full[SCAN_PATH_MAX];

    if (!d)
        return -1;

    de = readdir(d);
    if (!de)
        return -1;

    snprintf(e->name, sizeof e->name, "%s", de->d_name);
    snprintf(full, sizeof full, "%s/%s", lv->local, de->d_name);

    /* stat rather than d_type: not every filesystem fills d_type in, and a
       symlink should be followed to what it points at. */
    if (stat(full, &st) != 0) {
        e->is_dir = 0;
        e->size = 0;
        e->mtime = 0;
        return 0;                   /* reported, then skipped as empty */
    }

    e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    e->size = (tth_u64)st.st_size;
    e->mtime = (unsigned long)st.st_mtime;
    return 0;
}

static void level_close(scan_level *lv)
{
    if (lv->handle)
        closedir((DIR *)lv->handle);
    lv->handle = NULL;
    lv->open = 0;
}

#endif
