/* userlist.c - SID-indexed user table. */
#include "userlist.h"

#include <stdlib.h>
#include <string.h>

#define TOMBSTONE ((unsigned long)-1)

#define INITIAL_CAP     64
#define INITIAL_IDX_CAP 256

/* SIDs are assigned sequentially by most hubs, so the low bits alone would
   cluster badly after a rehash. Multiply by a 32-bit odd constant (Knuth)
   and take the high bits. */
static unsigned int sid_hash(adc_sid sid, unsigned int mask)
{
    unsigned long h = (unsigned long)sid * 2654435761UL;
    return (unsigned int)((h >> 11) & mask);
}

static int idx_insert(adc_userlist *ul, adc_sid sid, unsigned int slot);
static int idx_grow(adc_userlist *ul);

void adc_userlist_init(adc_userlist *ul)
{
    memset(ul, 0, sizeof *ul);
}

void adc_userlist_free(adc_userlist *ul)
{
    free(ul->users);
    free(ul->idx);
    memset(ul, 0, sizeof *ul);
}

void adc_userlist_clear(adc_userlist *ul)
{
    unsigned int i;

    for (i = 0; i < ul->cap; i++)
        ul->users[i].in_use = 0;
    ul->count = 0;

    if (ul->idx)
        memset(ul->idx, 0, ul->idx_cap * sizeof *ul->idx);
    ul->idx_used = 0;
}

/* Returns the index slot holding sid, or -1. */
static long idx_lookup(adc_userlist *ul, adc_sid sid)
{
    unsigned int mask, pos, probes;

    if (!ul->idx || ul->idx_cap == 0)
        return -1;

    mask = ul->idx_cap - 1;
    pos = sid_hash(sid, mask);

    for (probes = 0; probes < ul->idx_cap; probes++) {
        unsigned long e = ul->idx[pos];

        if (e == 0)
            return -1;                      /* free slot: not present */
        if (e != TOMBSTONE) {
            unsigned int slot = (unsigned int)(e - 1);
            if (slot < ul->cap && ul->users[slot].in_use &&
                ul->users[slot].sid == sid)
                return (long)pos;
        }
        pos = (pos + 1) & mask;
    }
    return -1;
}

static int idx_grow(adc_userlist *ul)
{
    unsigned int newcap = ul->idx_cap ? ul->idx_cap * 2 : INITIAL_IDX_CAP;
    unsigned long *old = ul->idx;
    unsigned int oldcap = ul->idx_cap;
    unsigned long *fresh = (unsigned long *)calloc(newcap, sizeof *fresh);
    unsigned int i;

    if (!fresh)
        return -1;

    ul->idx = fresh;
    ul->idx_cap = newcap;
    ul->idx_used = 0;

    /* Rehash live entries only -- this is where tombstones get collected. */
    for (i = 0; i < oldcap; i++) {
        unsigned long e = old ? old[i] : 0;
        unsigned int slot;

        if (e == 0 || e == TOMBSTONE)
            continue;
        slot = (unsigned int)(e - 1);
        if (slot < ul->cap && ul->users[slot].in_use)
            idx_insert(ul, ul->users[slot].sid, slot);
    }

    free(old);
    return 0;
}

static int idx_insert(adc_userlist *ul, adc_sid sid, unsigned int slot)
{
    unsigned int mask, pos, probes;

    /* Keep the load factor under 1/2: open addressing degrades badly past
       that, and we are optimising for the login flood. */
    if (ul->idx_cap == 0 || (ul->idx_used + 1) * 2 >= ul->idx_cap) {
        if (idx_grow(ul) != 0)
            return -1;
    }

    mask = ul->idx_cap - 1;
    pos = sid_hash(sid, mask);

    for (probes = 0; probes < ul->idx_cap; probes++) {
        unsigned long e = ul->idx[pos];
        if (e == 0 || e == TOMBSTONE) {
            ul->idx[pos] = (unsigned long)slot + 1;
            ul->idx_used++;
            return 0;
        }
        pos = (pos + 1) & mask;
    }
    return -1;
}

adc_user *adc_userlist_find(adc_userlist *ul, adc_sid sid)
{
    long pos = idx_lookup(ul, sid);

    if (pos < 0)
        return NULL;
    return &ul->users[ul->idx[pos] - 1];
}

static int ci_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

adc_user *adc_userlist_find_nick(adc_userlist *ul, const char *nick)
{
    unsigned int i;

    for (i = 0; i < ul->cap; i++)
        if (ul->users[i].in_use && ci_equal(ul->users[i].nick, nick))
            return &ul->users[i];
    return NULL;
}

static int users_grow(adc_userlist *ul)
{
    unsigned int newcap = ul->cap ? ul->cap * 2 : INITIAL_CAP;
    adc_user *fresh = (adc_user *)realloc(ul->users, newcap * sizeof *fresh);

    if (!fresh)
        return -1;

    memset(fresh + ul->cap, 0, (newcap - ul->cap) * sizeof *fresh);
    ul->users = fresh;
    ul->cap = newcap;
    return 0;
}

adc_user *adc_userlist_get(adc_userlist *ul, adc_sid sid, int *created)
{
    adc_user *u = adc_userlist_find(ul, sid);
    unsigned int i;

    if (created)
        *created = 0;

    if (u)
        return u;

    /* Reuse a hole before growing: users come and go constantly on a busy
       hub, and realloc moving a 640KB array is not free. */
    for (i = 0; i < ul->cap; i++)
        if (!ul->users[i].in_use)
            break;

    if (i == ul->cap) {
        if (users_grow(ul) != 0)
            return NULL;
        /* i now points at the first fresh slot. */
    }

    u = &ul->users[i];
    memset(u, 0, sizeof *u);
    u->sid = sid;
    u->in_use = 1;

    /* Passive until proven otherwise. Being dialable is a positive claim
       (TCP4 in SU); assuming it by default would have us send CTMs into
       nowhere and report a peer as reachable when nothing said so. */
    u->passive = 1;

    if (idx_insert(ul, sid, i) != 0) {
        u->in_use = 0;
        return NULL;
    }

    ul->count++;
    if (created)
        *created = 1;
    return u;
}

int adc_userlist_remove(adc_userlist *ul, adc_sid sid)
{
    long pos = idx_lookup(ul, sid);
    unsigned int slot;

    if (pos < 0)
        return 0;

    slot = (unsigned int)(ul->idx[pos] - 1);
    ul->users[slot].in_use = 0;

    /* Tombstone rather than clear: clearing would break probe chains for
       any SID that collided with this one. */
    ul->idx[pos] = TOMBSTONE;

    if (ul->count > 0)
        ul->count--;
    return 1;
}

adc_user *adc_userlist_at(adc_userlist *ul, unsigned int index)
{
    unsigned int i, seen = 0;

    for (i = 0; i < ul->cap; i++) {
        if (!ul->users[i].in_use)
            continue;
        if (seen == index)
            return &ul->users[i];
        seen++;
    }
    return NULL;
}

tiger_u64 adc_userlist_total_share(adc_userlist *ul)
{
    tiger_u64 total = 0;
    unsigned int i;

    for (i = 0; i < ul->cap; i++)
        if (ul->users[i].in_use)
            total += ul->users[i].share;
    return total;
}
