/* index.c - the share index, its string pool, and search matching. */
#include "index.h"
#include "../common.h"
#include "tthlcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILES_INITIAL 256
#define POOL_INITIAL  8192
#define TTH_IDX_INITIAL 512     /* power of two; grows with the share */
#define MATCH_TERMS     12      /* AN/NO terms handled on the fast path */

static void idx_empty(share_index *ix);

int share_init(share_index *ix)
{
    memset(ix, 0, sizeof *ix);

    ix->files = (share_file *)malloc(FILES_INITIAL * sizeof *ix->files);
    ix->pool = (char *)malloc(POOL_INITIAL);
    if (!ix->files || !ix->pool) {
        free(ix->files);
        free(ix->pool);
        memset(ix, 0, sizeof *ix);
        return -1;
    }
    ix->cap = FILES_INITIAL;
    ix->pool_cap = POOL_INITIAL;

    /* Offset 0 is an empty string, so a zeroed record reads as "" rather
       than pointing at the first real name. */
    ix->pool[0] = '\0';
    ix->pool_len = 1;
    return 0;
}

void share_free(share_index *ix)
{
    free(ix->files);
    free(ix->pool);
    free(ix->tth_idx);
    memset(ix, 0, sizeof *ix);
}

void share_clear(share_index *ix)
{
    ix->count = 0;
    ix->pool_len = 1;
    if (ix->pool)
        ix->pool[0] = '\0';
    ix->total_size = 0;
    ix->hashed_count = 0;
    ix->hashed_size = 0;
    idx_empty(ix);
}

int share_add_root(share_index *ix, const char *path)
{
    size_t n;

    if (!path || !*path)
        return -1;
    if (ix->nroot >= SHARE_MAX_ROOTS)
        return -1;

    n = strlen(path);
    if (n >= SHARE_ROOT_MAX)
        return -1;

    memcpy(ix->roots[ix->nroot], path, n + 1);
    ix->nroot++;
    return 0;
}

/* --------------------------------------------------------- string pool */

static unsigned long pool_add(share_index *ix, const char *s)
{
    size_t n = strlen(s) + 1;
    unsigned long off;

    if (ix->pool_len + n > ix->pool_cap) {
        unsigned long want = ix->pool_cap;
        char *fresh;

        while (want < ix->pool_len + n)
            want *= 2;
        fresh = (char *)realloc(ix->pool, want);
        if (!fresh)
            return 0;              /* offset 0 is the empty string */
        ix->pool = fresh;
        ix->pool_cap = want;
    }

    off = ix->pool_len;
    memcpy(ix->pool + off, s, n);
    ix->pool_len += n;
    return off;
}

const char *share_str(const share_index *ix, unsigned long off)
{
    if (!ix->pool || off >= ix->pool_len)
        return "";
    return ix->pool + off;
}

const char *share_name(const share_index *ix, const share_file *f)
{
    return share_str(ix, f->name_off);
}

const char *share_virt(const share_index *ix, const share_file *f)
{
    return share_str(ix, f->virt_off);
}

const char *share_local(const share_index *ix, const share_file *f)
{
    return share_str(ix, f->local_off);
}

/* ----------------------------------------------------------- hash index
 *
 * A TTH is already a hash, so its leading bytes are as well distributed as
 * anything we could compute from them: the bucket is just the first four
 * bytes, masked. Nothing clever, and nothing to get wrong. */

static unsigned long tth_bucket(const unsigned char *tth)
{
    return ((unsigned long)tth[0]) |
           ((unsigned long)tth[1] << 8) |
           ((unsigned long)tth[2] << 16) |
           ((unsigned long)tth[3] << 24);
}

static void idx_empty(share_index *ix)
{
    if (ix->tth_idx)
        memset(ix->tth_idx, 0,
               (size_t)ix->tth_cap * sizeof *ix->tth_idx);
    ix->tth_used = 0;
}

/* Place slot+1 in the table. The caller guarantees room. */
static void idx_place(share_index *ix, const unsigned char *tth,
                      unsigned int slot)
{
    unsigned int mask = ix->tth_cap - 1;
    unsigned int i = (unsigned int)(tth_bucket(tth) & mask);

    while (ix->tth_idx[i] != 0)
        i = (i + 1) & mask;

    ix->tth_idx[i] = (unsigned long)slot + 1;
    ix->tth_used++;
}

/* Grow (or create) the table and reinsert what is already hashed. Returns
   -1 if it could not be allocated, in which case lookups fall back to a
   scan rather than failing. */
static int idx_grow(share_index *ix)
{
    unsigned int want = ix->tth_cap ? ix->tth_cap * 2 : TTH_IDX_INITIAL;
    unsigned long *fresh;
    unsigned int i;

    fresh = (unsigned long *)calloc(want, sizeof *fresh);
    if (!fresh)
        return -1;

    free(ix->tth_idx);
    ix->tth_idx = fresh;
    ix->tth_cap = want;
    ix->tth_used = 0;

    for (i = 0; i < ix->count; i++)
        if (ix->files[i].hashed)
            idx_place(ix, ix->files[i].tth, i);

    return 0;
}

/* ------------------------------------------------------------- records */

static int files_grow(share_index *ix)
{
    unsigned int want = ix->cap ? ix->cap * 2 : FILES_INITIAL;
    share_file *fresh = (share_file *)realloc(ix->files,
                                              (size_t)want * sizeof *fresh);

    if (!fresh)
        return -1;
    ix->files = fresh;
    ix->cap = want;
    return 0;
}

/* The set of letters a string contains, folded to lower case. Bit 0 is
   'a', bit 25 is 'z', bit 26 stands for any digit; everything else --
   punctuation, separators, CP437 high bytes -- is ignored, which costs
   filtering power but can never produce a false negative. */
static unsigned long letters_of(const char *s)
{
    unsigned long m = 0;

    for (; *s; s++) {
        int c = (unsigned char)*s;

        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c >= 'a' && c <= 'z')
            m |= 1UL << (c - 'a');
        else if (c >= '0' && c <= '9')
            m |= 1UL << 26;
    }
    return m;
}

static const char *basename_of(const char *path)
{
    const char *last = path;

    for (; *path; path++)
        if (*path == '/' || *path == '\\')
            last = path + 1;
    return last;
}

share_file *share_add_file(share_index *ix, const char *virt,
                           const char *local, tth_u64 size,
                           unsigned long mtime)
{
    share_file *f;
    unsigned long v_off, l_off, n_off;

    if (!virt || !local)
        return NULL;

    /* Too big to address. Sharing it would mean advertising a file we
       cannot serve past the 2GB mark -- not an error at the time, but
       wrong bytes later. See DOSYPP_MAX_FILE. */
    if (size > (tth_u64)DOSYPP_MAX_FILE)
        return NULL;

    if (ix->count >= ix->cap && files_grow(ix) != 0)
        return NULL;

    /* Intern before taking the record pointer: a realloc inside pool_add
       cannot move ix->files, but taking the pointer first and then growing
       the pool is the kind of ordering that rots later. */
    v_off = pool_add(ix, virt);
    l_off = pool_add(ix, local);
    n_off = pool_add(ix, basename_of(virt));
    if (v_off == 0 || l_off == 0 || n_off == 0)
        return NULL;               /* out of memory */

    f = &ix->files[ix->count++];
    memset(f, 0, sizeof *f);
    f->virt_off = v_off;
    f->local_off = l_off;
    f->name_off = n_off;
    f->size = size;
    f->mtime = mtime;
    f->letters = letters_of(virt);

    ix->total_size += size;
    return f;
}

void share_set_tth(share_index *ix, share_file *f,
                   const unsigned char *tth)
{
    if (f->hashed)
        return;                    /* accounting must not double-count */

    memcpy(f->tth, tth, TTH_HASH_LEN);
    f->hashed = 1;
    ix->hashed_count++;
    ix->hashed_size += f->size;

    /* Keep the table at or below three-quarters full: an open-addressed
       table degrades to a scan as it fills, which is the thing this exists
       to avoid.
     *
     * The growth path reinserts every hashed record, and this one is now
     * among them -- so it must not also be placed by hand, or the table
     * ends up holding it twice. */
    if (ix->tth_used + 1 > ix->tth_cap - (ix->tth_cap / 4)) {
        if (idx_grow(ix) != 0) {
            /* Out of memory. The table cannot take this hash and cannot be
               left without it: a missing entry makes a hashed file
               unfindable, because a free bucket ends the probe. Throwing
               the table away is the safe failure -- lookups become a scan,
               which is slow and correct. */
            free(ix->tth_idx);
            ix->tth_idx = NULL;
            ix->tth_cap = 0;
            ix->tth_used = 0;
        }
    } else {
        idx_place(ix, tth, (unsigned int)(f - ix->files));
    }
}

void share_unhash_all(share_index *ix)
{
    unsigned int i;

    for (i = 0; i < ix->count; i++)
        ix->files[i].hashed = 0;

    ix->hashed_count = 0;
    ix->hashed_size = 0;
    idx_empty(ix);
}

share_file *share_find_tth(share_index *ix, const unsigned char *tth)
{
    unsigned int mask, i, probes;

    /* No table (nothing hashed yet, or the allocation failed): fall back to
       a scan, because answering correctly and slowly beats not answering. */
    if (!ix->tth_idx) {
        for (i = 0; i < ix->count; i++)
            if (ix->files[i].hashed &&
                memcmp(ix->files[i].tth, tth, TTH_HASH_LEN) == 0)
                return &ix->files[i];
        return NULL;
    }

    mask = ix->tth_cap - 1;
    i = (unsigned int)(tth_bucket(tth) & mask);

    for (probes = 0; probes <= mask; probes++) {
        unsigned long slot = ix->tth_idx[i];
        share_file *f;

        if (slot == 0)
            return NULL;           /* a free bucket ends the chain */

        f = &ix->files[slot - 1];
        if (f->hashed && memcmp(f->tth, tth, TTH_HASH_LEN) == 0)
            return f;

        i = (i + 1) & mask;
    }
    return NULL;
}

share_file *share_find_tth_b32(share_index *ix, const char *b32)
{
    unsigned char raw[TTH_HASH_LEN + 4];

    if (!b32 || base32_decode(raw, sizeof raw, b32) != TTH_HASH_LEN)
        return NULL;
    return share_find_tth(ix, raw);
}

share_file *share_next_unhashed(share_index *ix)
{
    unsigned int i;

    for (i = 0; i < ix->count; i++)
        if (!ix->files[i].hashed)
            return &ix->files[i];
    return NULL;
}

/* ------------------------------------------------------------- leaves */

typedef struct {
    FILE *out;
    long  written;
    int   failed;
} leaf_sink;

static void leaf_out(void *user, const unsigned char *hash)
{
    leaf_sink *s = (leaf_sink *)user;

    if (s->failed)
        return;
    if (fwrite(hash, 1, TTH_HASH_LEN, s->out) != TTH_HASH_LEN) {
        s->failed = 1;
        return;
    }
    s->written += TTH_HASH_LEN;
}

long share_write_tthl(share_index *ix, share_file *f, const char *path,
                      int *level_out)
{
    leaf_sink sink;
    tth_ctx ctx;
    FILE *in;
    unsigned char buf[4096];
    unsigned char root[TTH_HASH_LEN];
    size_t n;
    int level = 0;

    if (!f || !f->hashed)
        return -1;

    /* Already computed once? Leaves cost a full hash pass -- two seconds
       per megabyte on a 486 -- and two peers asking for the same file is
       the ordinary case, not the exception.
     *
       The cache is keyed on the root hash, so it needs no invalidation: a
       file that changed has a different root and its old record is simply
       never asked for again. */
    {
        long cached = tthl_cache_get(TTHL_CACHE_FILE, f->tth, path,
                                     level_out);

        if (cached > 0)
            return cached;
    }

    /* Pick a level whose node count fits in a few kilobytes. Each node at
       level n covers 1024 << n bytes, so this doubles the coverage until
       there are at most 1024 of them -- 24KB on the wire, whatever the
       file's size. */
    while (level < 24) {
        tth_u64 span = (tth_u64)TTH_LEAF_SIZE << level;
        tth_u64 count = (f->size + span - 1) / (span ? span : 1);

        if (count <= 1024)
            break;
        level++;
    }

    in = fopen(share_local(ix, f), "rb");
    if (!in)
        return -1;

    sink.out = fopen(path, "wb");
    sink.written = 0;
    sink.failed = 0;
    if (!sink.out) {
        fclose(in);
        return -1;
    }

    tth_init(&ctx, leaf_out, &sink, level);

    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        tth_update(&ctx, buf, n);

    tth_final(&ctx, root);

    fclose(in);
    if (fclose(sink.out) != 0 || sink.failed) {
        remove(path);
        return -1;
    }

    /* The file changed under us since it was hashed: serving leaves that do
       not match the root we advertised would make the asker reject a good
       file later, with no way to tell why. */
    if (memcmp(root, f->tth, TTH_HASH_LEN) != 0) {
        remove(path);
        return -1;
    }

    /* Keep them. A failure here is survivable -- the leaves are already
       written and the next request just recomputes them -- so it is not
       reported upwards. */
    {
        FILE *back = fopen(path, "rb");

        if (back) {
            unsigned char *all =
                (unsigned char *)malloc((size_t)sink.written);

            if (all) {
                if (fread(all, 1, (size_t)sink.written, back) ==
                    (size_t)sink.written)
                    tthl_cache_put(TTHL_CACHE_FILE, f->tth, level, all,
                                   sink.written);
                free(all);
            }
            fclose(back);
        }
    }

    if (level_out)
        *level_out = level;
    return sink.written;
}

/* ------------------------------------------------------------ matching */

#define LOWER(c) (((c) >= 'A' && (c) <= 'Z') ? (c) + 32 : (c))

/* Substring search against a needle that has already been folded to lower
   case, with a tight scan for its first character.
 *
 * The folding matters more than it looks: share_match calls this once per
 * file, so folding the needle inside it means re-folding the same search
 * term ten thousand times. On a 486 that was a third of a second to answer
 * one search -- see test/dos/searchbench.c. */
static int ci_find(const char *haystack, const char *lowered, size_t nlen)
{
    int first;

    if (nlen == 0)
        return 1;

    first = lowered[0];

    for (;;) {
        size_t i;

        /* Skip to a plausible start. This is the loop that runs over most
           of the path, so it stays as small as it can be. */
        while (*haystack && LOWER(*haystack) != first)
            haystack++;
        if (*haystack == '\0')
            return 0;

        for (i = 1; i < nlen; i++) {
            int a = haystack[i];
            if (a == '\0')
                return 0;
            if (LOWER(a) != lowered[i])
                break;
        }
        if (i == nlen)
            return 1;

        haystack++;
    }
}

/* Fold `src` into `dst` (at most dstsz including the NUL). Returns the
   length, or -1 if it would not fit. */
static int lower_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = 0;

    while (src[n]) {
        if (n + 1 >= dstsz)
            return -1;
        dst[n] = (char)LOWER((unsigned char)src[n]);
        n++;
    }
    dst[n] = '\0';
    return (int)n;
}

/* The longest term ci_contains can evaluate. Callers that care about the
   difference between "absent" and "could not tell" check the length
   themselves -- see the NO handling in share_match. */
#define CI_NEEDLE_MAX 127

static int ci_contains(const char *haystack, const char *needle)
{
    char low[CI_NEEDLE_MAX + 1];

    if (lower_copy(low, sizeof low, needle) < 0)
        return 0;                  /* cannot tell; see CI_NEEDLE_MAX */
    return ci_find(haystack, low, strlen(low));
}

static int ends_with_any(const char *path, const char *const *exts, int n)
{
    size_t plen = strlen(path);
    int i;

    for (i = 0; i < n; i++) {
        size_t elen = strlen(exts[i]);
        if (plen >= elen && ci_contains(path + plen - elen, exts[i]))
            return 1;
    }
    return 0;
}

/* The ADC group codes a client can ask for, by extension. Group 7 is
   directories, which we do not advertise: a share of files has none to
   offer and claiming otherwise invites a browse we cannot serve. */
static int group_matches(const char *path, const char *gr)
{
    static const char *const audio[] =
        { ".mp3", ".mod", ".s3m", ".xm", ".it", ".wav", ".mid", ".ogg" };
    static const char *const archive[] =
        { ".zip", ".arj", ".lzh", ".lha", ".rar", ".gz", ".z", ".tar" };
    static const char *const document[] =
        { ".txt", ".doc", ".nfo", ".diz", ".1st", ".me", ".ans" };
    static const char *const executable[] =
        { ".exe", ".com", ".bat", ".dll" };
    static const char *const picture[] =
        { ".gif", ".jpg", ".jpeg", ".pcx", ".bmp", ".png", ".tga" };
    static const char *const video[] =
        { ".avi", ".mpg", ".mpeg", ".flc", ".fli", ".mov" };

#define GRP(a) ends_with_any(path, a, (int)(sizeof a / sizeof a[0]))

    switch (gr[0]) {
    case '1': return GRP(audio);
    case '2': return GRP(archive);
    case '3': return GRP(document);
    case '4': return GRP(executable);
    case '5': return GRP(picture);
    case '6': return GRP(video);
    case '7': return 0;            /* directories: we share none */
    default:  return 1;            /* unknown group: do not filter */
    }

#undef GRP
}

/* Saturating, from tokenize.c. These are the GE and LE fields of somebody
   else's search: a wrapped bound would match files it should not. */
#define parse_u64(s) adc_u64_parse(s)

int share_match(share_index *ix, const adc_msg *m, share_file **out,
                int max)
{
    const char *tr = adc_field(m, "TR");
    const char *gr = adc_field(m, "GR");
    const char *ge = adc_field(m, "GE");
    const char *le = adc_field(m, "LE");
    const char *eq = adc_field(m, "EQ");
    unsigned int i;
    int n = 0;
    int hdr;
    unsigned long need = 0;        /* letters every AN term requires */

    /* Search terms, folded once. Sized for a real search: ADC searches are
       a few words, and anything beyond this takes the slow path. */
    struct {
        char   text[72];
        size_t len;
        int    no;
    } term[MATCH_TERMS];
    int nterm = 0;
    int slow = 0;

    if (max <= 0)
        return 0;

    /* An exact hash search is answered by at most one file, and only if we
       have hashed it. */
    if (tr) {
        share_file *f = share_find_tth_b32(ix, tr);
        if (f) {
            out[0] = f;
            return 1;
        }
        return 0;
    }

    hdr = adc_hdr_params(m->type);

    /* Prepare the terms once, not once per file. Both halves of this
       matter at 10,000 files: the letter mask rejects most files without a
       string compare, and the folded copies mean the term is not lowered
       again for every file it is tested against.
     *
     * NO terms are kept out of the mask deliberately: a file missing their
       letters simply does not contain them, which is a match, not a
       rejection. */
    {
        int j;

        for (j = hdr; j < m->nparam; j++) {
            const char *p = m->param[j];
            int is_no;

            if (p[0] == 'A' && p[1] == 'N')
                is_no = 0;
            else if (p[0] == 'N' && p[1] == 'O')
                is_no = 1;
            else
                continue;

            if (!is_no)
                need |= letters_of(p + 2);

            if (nterm < MATCH_TERMS) {
                int len = lower_copy(term[nterm].text,
                                     sizeof term[nterm].text, p + 2);
                if (len >= 0) {
                    term[nterm].len = (size_t)len;
                    term[nterm].no = is_no;
                    nterm++;
                    continue;
                }
            }

            /* Did not fit -- a term longer than any name, or more terms
               than a sane search has. Rare enough to be worth handling by
               falling back to the plain path for everything rather than
               carrying a second mechanism through the hot loop. Ignoring
               it is not an option: a dropped NO term over-matches. */
            slow = 1;
        }
    }

    for (i = 0; i < ix->count && n < max; i++) {
        share_file *f = &ix->files[i];
        const char *path = share_virt(ix, f);
        int j;
        int ok = 1;

        /* Never answer with a file we have not hashed: the asker addresses
           downloads by hash, so advertising one we cannot name would
           produce a request we must refuse. */
        if (!f->hashed)
            continue;

        /* The cheap rejection, before any string work at all. */
        if ((need & ~f->letters) != 0)
            continue;

        if (ge && f->size < parse_u64(ge))
            continue;
        if (le && f->size > parse_u64(le))
            continue;
        if (eq && f->size != parse_u64(eq))
            continue;
        if (gr && !group_matches(path, gr))
            continue;

        /* Every AN term must appear somewhere in the path; any NO term
           disqualifies. Both are matched against the whole virtual path, so
           a directory name counts -- which is what people expect when they
           search for "warez". */
        for (j = 0; j < nterm && ok; j++) {
            int found = ci_find(path, term[j].text, term[j].len);

            /* A NO term is disqualified by being found; an AN term by not
               being found. */
            if (term[j].no ? found : !found)
                ok = 0;
        }

        /* The pathological case: terms that did not fit above, applied the
           plain way so nothing is silently dropped. */
        for (j = hdr; slow && j < m->nparam && ok; j++) {
            const char *p = m->param[j];

            if (p[0] == 'A' && p[1] == 'N') {
                if (!ci_contains(path, p + 2))
                    ok = 0;
            } else if (p[0] == 'N' && p[1] == 'O') {
                /* A term too long to evaluate must not be assumed absent:
                   that would answer with a file the asker explicitly
                   excluded. Declining to answer is the safe direction. */
                if (strlen(p + 2) > CI_NEEDLE_MAX ||
                    ci_contains(path, p + 2))
                    ok = 0;
            }
        }
        if (!ok)
            continue;

        out[n++] = f;
    }

    return n;
}
