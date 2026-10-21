/* namemap.c - 8.3 names for files that do not have them. */
#include "namemap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char shortname[NAMEMAP_SHORT];
    char longname[NAMEMAP_LONG];
} nm_entry;

struct namemap {
    nm_entry *e;
    int       count;
    int       cap;
    char      path[128];
    unsigned long counter;      /* for generated names */
};

int namemap_init(namemap **nmp)
{
    namemap *nm = (namemap *)calloc(1, sizeof *nm);

    *nmp = NULL;
    if (!nm)
        return -1;

    nm->e = (nm_entry *)malloc(64 * sizeof *nm->e);
    if (!nm->e) {
        free(nm);
        return -1;
    }
    nm->cap = 64;
    *nmp = nm;
    return 0;
}

void namemap_free(namemap **nmp)
{
    namemap *nm = *nmp;

    if (!nm)
        return;
    free(nm->e);
    free(nm);
    *nmp = NULL;
}

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

static int grow(namemap *nm)
{
    int want = nm->cap * 2;
    nm_entry *fresh;

    if (want > NAMEMAP_MAX)
        want = NAMEMAP_MAX;
    if (nm->count >= want)
        return -1;

    fresh = (nm_entry *)realloc(nm->e, (size_t)want * sizeof *fresh);
    if (!fresh)
        return -1;
    nm->e = fresh;
    nm->cap = want;
    return 0;
}

static int add(namemap *nm, const char *shortname, const char *longname)
{
    nm_entry *e;

    if (nm->count >= nm->cap && grow(nm) != 0)
        return -1;

    e = &nm->e[nm->count++];
    snprintf(e->shortname, sizeof e->shortname, "%s", shortname);
    snprintf(e->longname, sizeof e->longname, "%s", longname);
    return 0;
}

const char *namemap_long(const namemap *nm, const char *shortname)
{
    int i;

    if (!nm || !shortname)
        return NULL;
    for (i = 0; i < nm->count; i++)
        if (ci_eq(nm->e[i].shortname, shortname))
            return nm->e[i].longname;
    return NULL;
}

const char *namemap_short(const namemap *nm, const char *longname)
{
    int i;

    if (!nm || !longname)
        return NULL;
    for (i = 0; i < nm->count; i++)
        if (strcmp(nm->e[i].longname, longname) == 0)
            return nm->e[i].shortname;
    return NULL;
}

/* ------------------------------------------------------------ squashing */

static int legal_83(unsigned char c)
{
    /* The DOS-legal set, upper case only. Deliberately conservative: a
       character DOS tolerates but COMMAND.COM cannot type is not useful. */
    if (c >= 'A' && c <= 'Z')
        return 1;
    if (c >= '0' && c <= '9')
        return 1;
    return strchr("_-!#$%&'()@^`{}~", (int)c) != NULL;
}

void namemap_squash(const char *longname, char *out)
{
    const char *dot = NULL;
    const char *p;
    char stem[9];
    char ext[4];
    int si = 0, ei = 0;

    out[0] = '\0';
    if (!longname || !*longname)
        return;

    /* The LAST dot is the extension: "archive.tar.gz" should keep "GZ". */
    for (p = longname; *p; p++)
        if (*p == '.')
            dot = p;

    for (p = longname; *p && (dot ? p < dot : 1) && si < 8; p++) {
        unsigned char c = (unsigned char)*p;

        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (legal_83(c))
            stem[si++] = (char)c;
        /* Spaces and anything else are dropped rather than replaced: an
           underscore-studded name is harder to read than a compressed one. */
    }
    stem[si] = '\0';

    if (dot) {
        for (p = dot + 1; *p && ei < 3; p++) {
            unsigned char c = (unsigned char)*p;

            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (legal_83(c))
                ext[ei++] = (char)c;
        }
    }
    ext[ei] = '\0';

    /* A name with no usable characters at all still needs something. */
    if (si == 0)
        snprintf(stem, sizeof stem, "FILE");

    if (ei > 0)
        snprintf(out, NAMEMAP_SHORT, "%s.%s", stem, ext);
    else
        snprintf(out, NAMEMAP_SHORT, "%s", stem);
}

/* ------------------------------------------------------------ assigning */

static int taken(namemap *nm, const char *candidate,
                 int (*exists)(const char *, void *), void *user)
{
    if (namemap_long(nm, candidate))
        return 1;
    if (exists && exists(candidate, user))
        return 1;
    return 0;
}

int namemap_assign(namemap *nm, const char *longname, char *out,
                   int (*exists)(const char *, void *), void *user)
{
    char base[NAMEMAP_SHORT];
    char ext[4];
    char stem[9];
    const char *dot;
    int attempt;

    if (!nm || !longname)
        return -1;

    /* Already mapped: the same file asked for twice must not get a second
       name, or the first download becomes unreachable. */
    {
        const char *have = namemap_short(nm, longname);
        if (have) {
            snprintf(out, NAMEMAP_SHORT, "%s", have);
            return 0;
        }
    }

    namemap_squash(longname, base);

    if (!taken(nm, base, exists, user)) {
        snprintf(out, NAMEMAP_SHORT, "%s", base);
        return add(nm, out, longname) == 0 ? 0 : -1;
    }

    /* Collision. Shorten the stem and append a counter: DOOM19S.ZIP becomes
       DOOM19~1.ZIP style, which is the convention anyone who used Windows
       95 will recognise. */
    dot = strchr(base, '.');
    if (dot) {
        snprintf(ext, sizeof ext, "%s", dot + 1);
        {
            size_t n = (size_t)(dot - base);
            if (n > 8)
                n = 8;
            memcpy(stem, base, n);
            stem[n] = '\0';
        }
    } else {
        ext[0] = '\0';
        snprintf(stem, sizeof stem, "%s", base);
    }

    for (attempt = 1; attempt <= 999; attempt++) {
        char cand[NAMEMAP_SHORT];
        char shortstem[9];
        int keep;
        char suffix[6];

        snprintf(suffix, sizeof suffix, "~%d", attempt);
        keep = 8 - (int)strlen(suffix);
        if (keep < 1)
            keep = 1;

        snprintf(shortstem, sizeof shortstem, "%.*s%s", keep, stem, suffix);

        if (ext[0])
            snprintf(cand, sizeof cand, "%s.%s", shortstem, ext);
        else
            snprintf(cand, sizeof cand, "%s", shortstem);

        if (!taken(nm, cand, exists, user)) {
            snprintf(out, NAMEMAP_SHORT, "%s", cand);
            return add(nm, out, longname) == 0 ? 0 : -1;
        }
    }

    /* Nothing derived worked. Fall back to a pure counter, which cannot
       collide with a derived name because of the leading 'D'. */
    for (attempt = 0; attempt < 100000; attempt++) {
        char cand[NAMEMAP_SHORT];

        snprintf(cand, sizeof cand, "D%07lu", nm->counter++);
        if (!taken(nm, cand, exists, user)) {
            snprintf(out, NAMEMAP_SHORT, "%s", cand);
            return add(nm, out, longname) == 0 ? 0 : -1;
        }
    }

    return -1;
}

/* ------------------------------------------------------------- the file */

int namemap_load(namemap *nm, const char *path)
{
    FILE *f;
    char line[320];
    int n = 0;

    if (!nm)
        return 0;

    snprintf(nm->path, sizeof nm->path, "%s", path ? path : NAMEMAP_FILE);

    f = fopen(nm->path, "r");
    if (!f)
        return 0;                   /* first run */

    while (fgets(line, sizeof line, f)) {
        char sh[NAMEMAP_SHORT];
        char lo[NAMEMAP_LONG];

        if (line[0] == ';' || line[0] == '\n')
            continue;

        /* "<short> <long with spaces>" -- the short name cannot contain a
           space, so one split is unambiguous. */
        if (sscanf(line, "%12s %127[^\n]", sh, lo) != 2)
            continue;               /* corrupt line: skip, do not fail */

        if (add(nm, sh, lo) != 0)
            break;                  /* full */
        n++;
    }

    fclose(f);
    return n;
}

int namemap_append(namemap *nm, const char *path, const char *shortname,
                   const char *longname)
{
    FILE *f;

    if (!nm)
        return -1;

    f = fopen(path ? path : nm->path, "a");
    if (!f)
        return -1;

    /* Written the moment a name is assigned, not at exit: a crash in
       between would leave a file on disk that nothing can identify. */
    fprintf(f, "%s %s\n", shortname, longname);

    if (fclose(f) != 0)
        return -1;
    return 0;
}
