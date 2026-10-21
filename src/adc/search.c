/* search.c - building BSCH, and coalescing DRES/URES into results. */
#include "search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ADC group codes for the GR field, in search_group order. */
static const char *GROUP_CODE[SEARCH_GROUP_COUNT] = {
    NULL,   /* ANY: no GR field at all */
    "1",    /* audio */
    "2",    /* archive */
    "3",    /* document */
    "4",    /* executable */
    "5",    /* picture */
    "6",    /* video */
    "7",    /* folder */
    NULL    /* TTH: TR instead of GR */
};

static const char *GROUP_NAME[SEARCH_GROUP_COUNT] = {
    "ANY", "AUDIO", "ARCHIVE", "DOCUMENT", "PROGRAM",
    "PICTURE", "VIDEO", "DIRECTORY", "BY HASH"
};

const char *search_group_name(search_group g)
{
    if (g < 0 || g >= SEARCH_GROUP_COUNT)
        return "ANY";
    return GROUP_NAME[g];
}

void search_init(search_state *ss)
{
    memset(ss, 0, sizeof *ss);
}

void search_fmt_size(tiger_u64 bytes, char *out, size_t outsz)
{
    static const char *unit[] = { "B", "K", "M", "G", "T" };
    tiger_u64 v = bytes;
    unsigned int u = 0;
    unsigned long frac = 0;

    while (v >= 1024 && u < 4) {
        frac = (unsigned long)((v % 1024) * 10 / 1024);
        v /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, outsz, "%lu%s", (unsigned long)v, unit[u]);
    else
        snprintf(out, outsz, "%lu.%lu%s", (unsigned long)v, frac, unit[u]);
}

/* --------------------------------------------------------- outbound */

/* Append " KEY" + escaped value. Returns 0, or -1 if it would not fit. */
static int append_field(char *out, size_t outsz, const char *key,
                        const char *value)
{
    char esc[ADC_LINE_MAX / 2];
    size_t have = strlen(out);
    size_t need;

    if (adc_escape(esc, sizeof esc, value) < 0)
        return -1;

    need = 1 + strlen(key) + strlen(esc);
    if (have + need + 1 >= outsz)
        return -1;

    strcat(out, " ");
    strcat(out, key);
    strcat(out, esc);
    return 0;
}

/* Split a phrase into AN terms. A hub matches each AN independently, so
   "doom shareware" must go out as two fields -- sending it as one means
   only files with that exact substring match, which is not what anyone
   typing two words expects. */
static int append_terms(char *out, size_t outsz, const char *key,
                        const char *phrase)
{
    char buf[128];
    char *tok;

    snprintf(buf, sizeof buf, "%s", phrase ? phrase : "");

    for (tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (*tok == '\0')
            continue;
        if (append_field(out, outsz, key, tok) != 0)
            return -1;
    }
    return 0;
}

int search_build(search_state *ss, adc_sid own_sid, const search_query *q,
                 char *out, size_t outsz, unsigned long now)
{
    char sid_text[ADC_SID_CHARS + 1];
    char num[32];

    if (!q)
        return -1;

    /* An empty search would ask the hub for everything, which gets you
       either nothing or a flood, and on some hubs a kick. */
    if (q->group == SEARCH_TTH) {
        if (q->tth[0] == '\0')
            return -1;
    } else if (q->terms[0] == '\0') {
        return -1;
    }

    /* Fresh token so late results from the previous search are discarded
       rather than mixed in. */
    ss->token_seq++;
    snprintf(ss->token, sizeof ss->token, "D%lu", ss->token_seq);

    ss->count = 0;
    ss->dropped = 0;
    ss->active = 1;
    ss->started = now;
    ss->query = *q;

    adc_sid_encode(own_sid, sid_text);
    snprintf(out, outsz, "BSCH %s", sid_text);

    if (append_field(out, outsz, "TO", ss->token) != 0)
        return -1;

    if (q->group == SEARCH_TTH) {
        /* An exact hash search needs nothing else, and adding terms would
           only narrow it wrongly. */
        if (append_field(out, outsz, "TR", q->tth) != 0)
            return -1;
        return 0;
    }

    if (append_terms(out, outsz, "AN", q->terms) != 0)
        return -1;
    if (q->exclude[0] && append_terms(out, outsz, "NO", q->exclude) != 0)
        return -1;

    if (GROUP_CODE[q->group]) {
        if (append_field(out, outsz, "GR", GROUP_CODE[q->group]) != 0)
            return -1;
    }

    if (q->min_size) {
        snprintf(num, sizeof num, "%lu", (unsigned long)q->min_size);
        if (append_field(out, outsz, "GE", num) != 0)
            return -1;
    }
    if (q->max_size) {
        snprintf(num, sizeof num, "%lu", (unsigned long)q->max_size);
        if (append_field(out, outsz, "LE", num) != 0)
            return -1;
    }

    return 0;
}

/* ---------------------------------------------------------- inbound */

/* Saturating, in tokenize.c: a size from the network that wraps would be
   acted on as a small file. */
#define parse_u64(s) adc_u64_parse(s)

/* The display name: the last path component. ADC paths use '/' but a peer
   sharing from Windows may send '\\', so both separate. */
static void basename_of(const char *path, char *out, size_t outsz)
{
    const char *p = path;
    const char *last = path;

    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            last = p + 1;

    snprintf(out, outsz, "%s", last);
    if (out[0] == '\0')          /* a path ending in a separator */
        snprintf(out, outsz, "%s", path);
}

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static search_result *find_by_tth(search_state *ss, const char *tth)
{
    int i;

    if (!tth || !*tth)
        return NULL;
    for (i = 0; i < ss->count; i++)
        if (strcmp(ss->results[i].tth, tth) == 0)
            return &ss->results[i];
    return NULL;
}

static void add_source(search_result *r, adc_sid sid, unsigned slots)
{
    int i;

    /* The same peer answering twice (TCP and UDP for one search) must not
       appear as two sources. */
    for (i = 0; i < r->nsource; i++)
        if (r->sources[i].sid == sid)
            return;

    if (r->nsource >= SR_MAX_SOURCES) {
        r->dropped_sources++;
        return;
    }

    r->sources[r->nsource].sid = sid;
    r->sources[r->nsource].slots = slots;
    r->nsource++;
}

int search_add_result(search_state *ss, const adc_msg *m)
{
    const char *token = adc_field(m, "TO");
    const char *fn = adc_field(m, "FN");
    const char *si = adc_field(m, "SI");
    const char *tr = adc_field(m, "TR");
    const char *sl = adc_field(m, "SL");
    adc_sid from = ADC_SID_NONE;
    search_result *r;
    unsigned slots;
    int is_dir;

    if (!fn)
        return 0;

    /* Results for a search we are no longer showing are discarded. A hub
       and its peers answer at their own pace, and a slow peer's reply can
       arrive after the user has searched for something else. */
    if (ss->token[0] && token && strcmp(token, ss->token) != 0)
        return 0;
    if (!ss->active && !ss->count)
        return 0;

    /* DRES carries the sender's SID first; URES carries a CID instead, in
       which case there is no SID to record. */
    if (m->type == 'D' || m->type == 'E') {
        if (m->nparam >= 1)
            adc_sid_decode(m->param[0], &from);
    }

    /* A directory result has no size and no hash. */
    is_dir = (si == NULL && tr == NULL);
    slots = sl ? (unsigned)strtoul(sl, NULL, 10) : 0;

    /* Coalesce by TTH: one row per file, many sources. Without this a
       popular file fills the screen with the same name. Directories have no
       TTH, so they are matched by name instead. */
    r = NULL;
    if (tr && *tr) {
        r = find_by_tth(ss, tr);
    } else {
        char base[SR_NAME_MAX];
        int i;

        basename_of(fn, base, sizeof base);
        for (i = 0; i < ss->count; i++)
            if (ss->results[i].is_dir &&
                strcmp(ss->results[i].name, base) == 0) {
                r = &ss->results[i];
                break;
            }
    }

    if (!r) {
        if (ss->count >= SR_MAX_RESULTS) {
            /* Bounded on purpose: a popular search on a busy hub can return
               thousands, and this has to survive on 16MB. */
            ss->dropped++;
            return 0;
        }
        r = &ss->results[ss->count++];
        memset(r, 0, sizeof *r);
        basename_of(fn, r->name, sizeof r->name);
        /* One path per result. A directory has no TTH, so this is the only
           way to ask for it later; for files it is display only. */
        copy_str(r->path, sizeof r->path, fn);
        copy_str(r->tth, sizeof r->tth, tr);
        r->size = parse_u64(si);
        r->is_dir = is_dir;
    }

    add_source(r, from, slots);
    return 1;
}

/* ---------------------------------------------------------- sorting */

static int cmp_name(const void *a, const void *b)
{
    const search_result *x = (const search_result *)a;
    const search_result *y = (const search_result *)b;
    int i = 0;

    /* Case-insensitive, so "Doom" and "doom" sort together rather than in
       two separate clumps. */
    for (;;) {
        int cx = x->name[i], cy = y->name[i];
        if (cx >= 'A' && cx <= 'Z') cx += 32;
        if (cy >= 'A' && cy <= 'Z') cy += 32;
        if (cx != cy)
            return cx - cy;
        if (cx == '\0')
            return 0;
        i++;
    }
}

static int cmp_size(const void *a, const void *b)
{
    const search_result *x = (const search_result *)a;
    const search_result *y = (const search_result *)b;

    if (x->size == y->size)
        return cmp_name(a, b);
    return (x->size < y->size) ? 1 : -1;      /* largest first */
}

static int cmp_sources(const void *a, const void *b)
{
    const search_result *x = (const search_result *)a;
    const search_result *y = (const search_result *)b;

    if (x->nsource != y->nsource)
        return y->nsource - x->nsource;       /* most sources first */
    return cmp_size(a, b);
}

void search_sort_by(search_state *ss, search_sort how)
{
    if (ss->count < 2)
        return;

    switch (how) {
    case SORT_NAME:
        qsort(ss->results, (size_t)ss->count, sizeof ss->results[0],
              cmp_name);
        break;
    case SORT_SIZE:
        qsort(ss->results, (size_t)ss->count, sizeof ss->results[0],
              cmp_size);
        break;
    default:
        qsort(ss->results, (size_t)ss->count, sizeof ss->results[0],
              cmp_sources);
        break;
    }
}

const char *search_sort_name(search_sort how)
{
    switch (how) {
    case SORT_NAME: return "NAME";
    case SORT_SIZE: return "SIZE";
    default:        return "SOURCES";
    }
}

/* ---------------------------------------------------------- tagging */

void search_tag_toggle(search_state *ss, int index)
{
    if (index < 0 || index >= ss->count)
        return;
    ss->results[index].tagged = !ss->results[index].tagged;
}

void search_tag_all(search_state *ss, int on)
{
    int i;

    for (i = 0; i < ss->count; i++)
        ss->results[i].tagged = on ? 1 : 0;
}

int search_tagged_count(const search_state *ss)
{
    int i, n = 0;

    for (i = 0; i < ss->count; i++)
        if (ss->results[i].tagged)
            n++;
    return n;
}
