/* queue.c - the download queue and the connection-setup dance. */
#include "queue.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hash/base32.h"
#include "../share/filelist.h"

int xq_init(xfer_queue *q, namemap *names)
{
    memset(q, 0, sizeof *q);
    q->names = names;
    return 0;
}

static void plan_release(xfer_queue *q, xq_request *r);

void xq_free(xfer_queue *q)
{
    int i;

    for (i = 0; i < XQ_MAX_DOWNLOADS; i++)
        if (q->down_used[i])
            peer_close(&q->down[i]);
    for (i = 0; i < XQ_MAX_UPLOADS; i++)
        if (q->up_used[i])
            peer_close(&q->up[i]);
    for (i = 0; i < XQ_MAX_DOWNLOADS; i++)
        free(q->plans[i]);
    memset(q, 0, sizeof *q);
}

const char *xq_status_name(xq_status s)
{
    switch (s) {
    case XQ_WAITING: return "QUEUED";
    case XQ_ASKING:  return "CONNECTING";
    case XQ_ACTIVE:  return "TRANSFER";
    case XQ_DONE:    return "COMPLETE";
    default:         return "FAILED";
    }
}

void xq_mint_token(xfer_queue *q, char *out, size_t outsz)
{
    /* Tokens only have to be unique to us, and they come back from the peer
       verbatim, so keep them short and ASCII. */
    snprintf(out, outsz, "T%lu", ++q->token_seq);
}

int xq_downloads_active(const xfer_queue *q)
{
    int i, n = 0;

    for (i = 0; i < XQ_MAX_DOWNLOADS; i++)
        if (q->down_used[i])
            n++;

    /* Claimed connections live in the upload array (see xq_poll), so they
       have to be counted here or the limit on simultaneous downloads would
       only apply to the ones we dialled. */
    for (i = 0; i < XQ_MAX_UPLOADS; i++)
        if (q->up_used[i] && !q->up[i].uploading)
            n++;

    return n;
}

int xq_uploads_active(const xfer_queue *q)
{
    int i, n = 0;

    for (i = 0; i < XQ_MAX_UPLOADS; i++)
        if (q->up_used[i])
            n++;
    return n;
}

/* Uploads actually moving bytes. This, not the number of open connections,
   is what a slot limit is about: a peer that connects and then asks for
   nothing must not keep a slot from someone who wants a file, and clients
   do sit on idle connections between requests. */
int xq_uploads_sending(const xfer_queue *q)
{
    int i, n = 0;

    for (i = 0; i < XQ_MAX_UPLOADS; i++)
        if (q->up_used[i] && q->up[i].state == PEER_SENDING)
            n++;
    return n;
}

int xq_free_upload_slots(const xfer_queue *q, int configured)
{
    int used = xq_uploads_sending(q);
    int left = configured - used;

    return left < 0 ? 0 : left;
}

/* ------------------------------------------------------------ requests */

xq_request *xq_add(xfer_queue *q, const search_result *r,
                   const char *download_dir)
{
    xq_request *req;
    char shortname[NAMEMAP_SHORT];
    int i;

    if (!r || r->is_dir)
        return NULL;                /* directories need a browse, not a get */
    if (!r->tth[0])
        return NULL;                /* without a hash we cannot ask for it */

    /* Too big to address: see DOSYPP_MAX_FILE. Refusing it up front is the
       difference between "that file is too big for this machine" and a
       download that runs for an hour and then verifies against the wrong
       bytes. */
    if (r->size > (tth_u64)DOSYPP_MAX_FILE)
        return NULL;

    /* Already queued? Asking twice would write the same file from two
       connections. */
    for (i = 0; i < q->nreq; i++)
        if (strcmp(q->reqs[i].tth, r->tth) == 0 &&
            q->reqs[i].status != XQ_FAILED)
            return NULL;

    if (q->nreq >= XQ_MAX_QUEUED)
        return NULL;

    req = &q->reqs[q->nreq++];
    memset(req, 0, sizeof *req);
    req->plan = -1;

    snprintf(req->name, sizeof req->name, "%s", r->name);
    snprintf(req->tth, sizeof req->tth, "%s", r->tth);
    req->size = r->size;
    req->status = XQ_WAITING;

    req->nsource = r->nsource;
    if (req->nsource > SR_MAX_SOURCES)
        req->nsource = SR_MAX_SOURCES;
    for (i = 0; i < req->nsource; i++)
        req->sources[i] = r->sources[i].sid;

    /* No sources means nobody to ask. Failing now is honest; leaving it
       WAITING would park it in the queue for ever with no explanation,
       which is how a stuck download looks like a hung client. */
    if (req->nsource == 0) {
        req->status = XQ_FAILED;
        snprintf(req->err, sizeof req->err, "no sources");
        q->failed++;
    }

    /* The local name is 8.3, because that is all DOS has. The real name
       lives in the map and is what gets displayed. */
    if (q->names &&
        namemap_assign(q->names, r->name, shortname, NULL, NULL) == 0) {
        namemap_append(q->names, NAMEMAP_FILE, shortname, r->name);
    } else {
        snprintf(shortname, sizeof shortname, "D%07d", q->nreq);
    }

    if (download_dir && *download_dir)
        snprintf(req->local, sizeof req->local, "%s%s%s", download_dir,
                 (download_dir[strlen(download_dir) - 1] == '/' ||
                  download_dir[strlen(download_dir) - 1] == '\\')
                     ? "" : "/",
                 shortname);
    else
        snprintf(req->local, sizeof req->local, "%s", shortname);

    return req;
}

/* Is there already a connection in flight to this node?
 *
   One at a time, deliberately. Several connections to the same peer need
   the MCN1 feature, which we do not advertise and do not implement, and a
   client that opens a second one anyway gets it closed: AirDC++ drops the
   duplicate, which cost a file list and a download in the same run. It is
   also pointless on a modem. */
/* Do we already have a connection, or an outstanding request for one, to
   this node?
 *
   One at a time per peer, deliberately. Several connections to the same
   peer need the MCN1 feature, which we do not advertise and do not
   implement, and a client that opens a second one anyway gets it closed:
   AirDC++ drops the duplicate, which cost a file list and a download in the
   same run. It is also pointless on a modem. */
static int busy_with(const xfer_queue *q, adc_sid sid)
{
    int i;

    for (i = 0; i < XQ_MAX_CONNS; i++)
        if (q->conns[i].live && q->conns[i].src == sid)
            return 1;
    return 0;
}

xq_request *xq_next_to_ask(xfer_queue *q, adc_sid *src)
{
    int i, j;

    if (xq_downloads_active(q) >= XQ_MAX_DOWNLOADS)
        return NULL;

    for (i = 0; i < q->nreq; i++) {
        xq_request *r = &q->reqs[i];
        xq_plan *pl = xq_plan_of(q, r);

        /* A plain request wants one connection; a segmented one wants a
           connection per source, because that is the whole point. */
        if (r->status == XQ_WAITING) {
            if (r->try_source >= r->nsource)
                continue;
        } else if (r->status == XQ_ACTIVE && pl != NULL) {
            long k;
            int parts_left = 0;

            for (k = 0; k < pl->nseg; k++)
                if (!pl->done[k] && !pl->busy[k]) {
                    parts_left = 1;
                    break;
                }
            if (!parts_left)
                continue;       /* every remaining part is being fetched */
        } else {
            continue;
        }

        /* The first source that has no connection of ours. For a plain
           request that is the cursor; for a segmented one it is any of
           them. */
        for (j = (pl ? 0 : r->try_source); j < r->nsource; j++) {
            if (busy_with(q, r->sources[j]))
                continue;

            if (src)
                *src = r->sources[j];
            return r;
        }
    }
    return NULL;
}

/* --------------------------------------------------- connection table */

static xq_conn *conn_by_token(xfer_queue *q, const char *token)
{
    int i;

    if (!token || !*token)
        return NULL;

    for (i = 0; i < XQ_MAX_CONNS; i++)
        if (q->conns[i].live && strcmp(q->conns[i].token, token) == 0)
            return &q->conns[i];
    return NULL;
}

int xq_note_asked(xfer_queue *q, xq_request *r, adc_sid src,
                  const char *token, unsigned long now)
{
    int i;

    if (!r || !token || !*token)
        return -1;

    for (i = 0; i < XQ_MAX_CONNS; i++) {
        if (q->conns[i].live)
            continue;

        q->conns[i].live = 1;
        q->conns[i].req = (int)(r - q->reqs);
        q->conns[i].src = src;
        q->conns[i].asked_at = now;
        snprintf(q->conns[i].token, sizeof q->conns[i].token, "%s", token);

        /* The request keeps the most recent token for display and for the
           queue file; matching goes through the table. */
        snprintf(r->token, sizeof r->token, "%s", token);
        return 0;
    }
    return -1;
}

void xq_forget(xfer_queue *q, const char *token)
{
    xq_conn *c = conn_by_token(q, token);

    if (c)
        c->live = 0;
}

int xq_expire_asks(xfer_queue *q, unsigned long now, unsigned long secs)
{
    int i, n = 0;

    for (i = 0; i < XQ_MAX_CONNS; i++) {
        xq_conn *c = &q->conns[i];
        xq_request *r;

        if (!c->live || now <= c->asked_at + secs)
            continue;

        r = &q->reqs[c->req];

        /* Only an attempt that never turned into a connection expires: one
           that is transferring has its own timeouts. */
        if (r->status != XQ_ASKING)
            continue;

        c->live = 0;
        n++;

        snprintf(r->err, sizeof r->err, "no answer");
        r->try_source++;
        if (xq_plan_of(q, r) != NULL && r->try_source >= r->nsource)
            r->try_source = 0;  /* a plan is worth going round again for */

        r->status = (r->try_source < r->nsource) ? XQ_WAITING : XQ_FAILED;
        if (r->status == XQ_FAILED)
            q->failed++;
    }
    return n;
}

xq_request *xq_by_token(xfer_queue *q, const char *token)
{
    xq_conn *c = conn_by_token(q, token);

    if (c)
        return &q->reqs[c->req];

    /* Fall back to the request's own last token. Connections adopted
       before this table existed -- and anything restored from the queue
       file -- still match. */
    {
        int i;

        if (!token || !*token)
            return NULL;

        for (i = 0; i < q->nreq; i++)
            if (strcmp(q->reqs[i].token, token) == 0)
                return &q->reqs[i];
    }
    return NULL;
}

/* -------------------------------------------------------- connections */

static int free_down_slot(xfer_queue *q)
{
    int i;

    for (i = 0; i < XQ_MAX_DOWNLOADS; i++)
        if (!q->down_used[i])
            return i;
    return -1;
}

static int free_up_slot(xfer_queue *q)
{
    int i;

    for (i = 0; i < XQ_MAX_UPLOADS; i++)
        if (!q->up_used[i])
            return i;
    return -1;
}

/* Where to resume from: the file on disk, every attempt, not what the
   queue remembers. Two reasons -- a request restored from the queue file
   may have been written to since, and an attempt that failed half way
   through this session has left more on disk than the queue knows about,
   so retrying from zero would throw it away.
 *
   A partial that is somehow already as long as the whole file is treated as
   unusable and fetched again: the alternative is calling a download
   finished without ever having hashed it. */
static tth_u64 resume_point(xq_request *r)
{
    FILE *partial = fopen(r->local, "rb");
    tth_u64 got = 0;

    if (partial) {
        if (fseek(partial, 0, SEEK_END) == 0) {
            long at = ftell(partial);

            if (at > 0)
                got = (tth_u64)at;
        }
        fclose(partial);
    }

    if (r->size > 0 && got >= r->size)
        got = 0;

    r->resume_from = got;
    return got;
}

/* Start the right kind of connection for a request. A list has no hash and
   no known size; a resumed file starts part way in. Both directions of the
   connection dance end up here, which is the point: the difference belongs
   in one place. */
static int start_for(xfer_queue *q, int slot, transport *t, xq_request *r,
                     const adc_identity *id, const peer_callbacks *cb,
                     unsigned long now)
{
    if (r->wants_list)
        return peer_start_list(&q->down[slot], t, id, r->token, r->local,
                               r->list_base, cb, now);

    /* Leaves first, if this request wants them: they make the download
       parallel and resume cheap, and they cost one small transfer. */
    if (xq_wants_tthl(q, r)) {
        r->tthl_pending = 1;
        return peer_start_tthl(&q->down[slot], t, id, r->token, r->tth,
                               TTHL_FILE, cb, now);
    }

    /* With a plan, the connection is handed segments one after another as
       each verifies. The first one goes out when the handshake finishes,
       which the caller does through xq_next_segment. */
    if (xq_plan_of(q, r) != NULL) {
        int rc = peer_start_download(&q->down[slot], t, id, r->token,
                                     r->tth, r->name, r->size, r->local,
                                     cb, now);

        if (rc != 0)
            return rc;

        /* Nothing is requested on the handshake: the segment assignment
           decides what to ask for, so the connection waits in IDLE. */
        q->down[slot].want = PEER_WANT_NOTHING;
        return 0;
    }

    if (peer_start_download(&q->down[slot], t, id, r->token, r->tth,
                            r->name, r->size, r->local, cb, now) != 0)
        return -1;

    q->down[slot].offset = resume_point(r);
    return 0;
}

xq_request *xq_add_list(xfer_queue *q, adc_sid src, const char *nick,
                        const char *local_path, const char *base)
{
    xq_request *req = NULL;
    int i;

    /* One list request per node. Asking twice would write the same file
       from two connections, and a list is the one request a user is likely
       to repeat impatiently. */
    for (i = 0; i < q->nreq; i++)
        if (q->reqs[i].wants_list && q->reqs[i].nsource == 1 &&
            q->reqs[i].sources[0] == src &&
            strcmp(q->reqs[i].list_base, base ? base : "") == 0 &&
            q->reqs[i].status != XQ_FAILED &&
            q->reqs[i].status != XQ_DONE)
            return &q->reqs[i];

    /* Reuse a finished slot before growing: browsing several people in a
       session must not fill the queue with spent list requests. */
    for (i = 0; i < q->nreq && !req; i++)
        if (q->reqs[i].wants_list &&
            (q->reqs[i].status == XQ_DONE || q->reqs[i].status == XQ_FAILED))
            req = &q->reqs[i];

    if (!req) {
        if (q->nreq >= XQ_MAX_QUEUED)
            return NULL;
        req = &q->reqs[q->nreq++];
    }

    memset(req, 0, sizeof *req);
    req->plan = -1;
    req->wants_list = 1;
    req->status = XQ_WAITING;
    req->nsource = 1;
    req->sources[0] = src;
    /* "/" is a partial list of the root, not the whole share: only NULL
       means everything. */
    if (base && base[0])
        snprintf(req->list_base, sizeof req->list_base, "%s", base);

    /* "/" is the root, and the root is the file list: calling that row
       "/ from Nodea" reads like a bug, which is what it was reported as.
       A subdirectory keeps its name, because that is the useful part. */
    if (req->list_base[0] && strcmp(req->list_base, "/") != 0)
        snprintf(req->name, sizeof req->name, "%s from %s", req->list_base,
                 (nick && nick[0]) ? nick : "that node");
    else
        snprintf(req->name, sizeof req->name, "file list from %s",
                 (nick && nick[0]) ? nick : "that node");
    snprintf(req->local, sizeof req->local, "%s", local_path);
    return req;
}

xq_request *xq_add_saved(xfer_queue *q, const char *tth, tth_u64 size,
                         const char *name, const char *local, tth_u64 got)
{
    xq_request *req;
    int i;

    if (!tth || !tth[0] || !local || !local[0])
        return NULL;

    for (i = 0; i < q->nreq; i++)
        if (strcmp(q->reqs[i].tth, tth) == 0 &&
            q->reqs[i].status != XQ_FAILED)
            return NULL;

    if (q->nreq >= XQ_MAX_QUEUED)
        return NULL;

    req = &q->reqs[q->nreq++];
    memset(req, 0, sizeof *req);
    req->plan = -1;

    snprintf(req->name, sizeof req->name, "%s",
             (name && name[0]) ? name : "(unnamed)");
    snprintf(req->tth, sizeof req->tth, "%s", tth);
    snprintf(req->local, sizeof req->local, "%s", local);
    req->size = size;
    req->resume_from = got;

    /* No sources yet, and that is not a failure: the hash is enough to go
       looking. Left WAITING with nothing to ask, which xq_next_to_ask
       skips until a search result arrives. */
    req->status = XQ_WAITING;
    return req;
}

int xq_add_source(xfer_queue *q, xq_request *r, adc_sid sid)
{
    int i;

    (void)q;
    if (!r)
        return 0;

    for (i = 0; i < r->nsource; i++)
        if (r->sources[i] == sid)
            return 1;

    if (r->nsource >= SR_MAX_SOURCES)
        return 0;

    r->sources[r->nsource++] = sid;
    return 1;
}

xq_request *xq_by_tth(xfer_queue *q, const char *tth)
{
    int i;

    if (!tth || !tth[0])
        return NULL;

    for (i = 0; i < q->nreq; i++)
        if (!q->reqs[i].wants_list &&
            q->reqs[i].status == XQ_WAITING &&
            strcmp(q->reqs[i].tth, tth) == 0)
            return &q->reqs[i];
    return NULL;
}

/* ------------------------------------------------- segmented downloads */

int xq_wants_tthl(const xfer_queue *q, const xq_request *r)
{
    (void)q;

    if (!r || r->wants_list || r->tth[0] == '\0')
        return 0;
    if (r->plan >= 0 || r->tthl_pending || r->tthl_refused)
        return 0;

    /* Worth it for any file big enough to have more than one leaf at the
       level share_write_tthl would pick -- which is to say any file worth
       segmenting. Under that, the whole-file path is simpler and the leaf
       fetch would be a round trip for nothing. */
    return r->size > TTH_LEAF_SIZE;
}

static int plan_slot(xfer_queue *q)
{
    int i;

    for (i = 0; i < XQ_MAX_DOWNLOADS; i++)
        if (!q->plans[i] || !q->plans[i]->in_use)
            return i;
    return -1;
}

/* Rebuild the Merkle root from a set of leaves: pair, prefix 0x01, and
   promote an odd one unchanged. Leaves that do not rebuild the root we
   advertise are not this file's leaves, whatever the peer says. */
static void fold_root(const unsigned char *leaves, long count,
                      unsigned char *out)
{
    static unsigned char level[XQ_MAX_SEGS][TTH_HASH_LEN];
    long n = count;
    long i;

    memcpy(level, leaves, (size_t)count * TTH_HASH_LEN);

    while (n > 1) {
        long w = 0;

        for (i = 0; i + 1 < n; i += 2) {
            tiger_ctx tc;
            unsigned char pfx = 0x01;

            tiger_init(&tc);
            tiger_update(&tc, &pfx, 1);
            tiger_update(&tc, level[i], TTH_HASH_LEN);
            tiger_update(&tc, level[i + 1], TTH_HASH_LEN);
            tiger_final(&tc, level[w]);
            w++;
        }
        if (i < n) {
            memmove(level[w], level[i], TTH_HASH_LEN);
            w++;
        }
        n = w;
    }

    memcpy(out, level[0], TTH_HASH_LEN);
}

/* The hash a segment must match: the fold of the leaves it covers. For a
   whole, aligned group that is exactly a node of the tree; for the short
   group at the end it is the fold of what remains, which is how the tree
   is built there too. */
static void segment_hash(const xq_plan *pl, long seg, unsigned char *out)
{
    long first = seg * pl->group;
    long count = pl->nleaf - first;

    if (count > pl->group)
        count = pl->group;

    fold_root((const unsigned char *)pl->leaf[first], count, out);
}

static tth_u64 segment_start(const xq_plan *pl, long seg)
{
    return (tth_u64)seg * pl->group * pl->span;
}

static tth_u64 segment_len(const xq_plan *pl, long seg)
{
    tth_u64 start = segment_start(pl, seg);
    tth_u64 len = (tth_u64)pl->group * pl->span;

    if (start + len > pl->size)
        len = pl->size - start;
    return len;
}

/* Is this segment already correct on disk? Hashing what is there is the
   only way to know: a partial file from a previous run may be short, may
   have holes, or may have come from a peer that lied. */
static int segment_on_disk(const xq_request *r, const xq_plan *pl, long i)
{
    FILE *f = fopen(r->local, "rb");
    tth_u64 start = segment_start(pl, i);
    tth_u64 len = segment_len(pl, i);
    tth_ctx ctx;
    unsigned char got[TTH_HASH_LEN];
    unsigned char want[TTH_HASH_LEN];
    unsigned char buf[4096];
    long at;

    if (!f)
        return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    at = ftell(f);
    if (at < 0 || (tth_u64)at < start + len) {
        fclose(f);
        return 0;               /* the file does not reach this segment */
    }
    if (fseek(f, (long)start, SEEK_SET) != 0) { fclose(f); return 0; }

    tth_init(&ctx, NULL, NULL, -1);
    while (len > 0) {
        size_t want = sizeof buf;
        size_t n;

        if ((tth_u64)want > len)
            want = (size_t)len;
        n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        tth_update(&ctx, buf, n);
        len -= (tth_u64)n;
    }
    fclose(f);

    if (len != 0)
        return 0;

    tth_final(&ctx, got);
    segment_hash(pl, i, want);
    return memcmp(got, want, TTH_HASH_LEN) == 0;
}

long xq_plan_load(xfer_queue *q, xq_request *r, const char *path)
{
    FILE *f;
    long bytes, count, i;
    int slot;
    xq_plan *pl;
    unsigned char root[TTH_HASH_LEN];
    unsigned char want[TTH_HASH_LEN];

    r->tthl_pending = 0;

    if (!r || r->tth[0] == '\0')
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    bytes = ftell(f);
    fclose(f);

    if (bytes <= 0 || bytes % TTH_HASH_LEN != 0)
        return 0;               /* not a leaf set; fall back */

    count = bytes / TTH_HASH_LEN;
    if (count > XQ_MAX_SEGS)
        return 0;               /* more parts than we plan for */

    slot = plan_slot(q);
    if (slot < 0)
        return 0;               /* all plans busy; whole-file will do */

    if (!q->plans[slot]) {
        q->plans[slot] = (xq_plan *)calloc(1, sizeof *q->plans[slot]);
        if (!q->plans[slot])
            return 0;
    }
    pl = q->plans[slot];
    memset(pl, 0, sizeof *pl);

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(pl->leaf, 1, (size_t)bytes, f) != (size_t)bytes) {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* What each leaf covers has to be derived: ADC does not send the level.
       The count and the size give it, and anything that does not divide
       cleanly means we misunderstood the set -- better to fall back than to
       verify against the wrong ranges. */
    pl->size = r->size;
    pl->nleaf = count;
    pl->span = TTH_LEAF_SIZE;
    while (pl->span * (tth_u64)count < r->size)
        pl->span *= 2;

    if (count > 1 && pl->span * (tth_u64)(count - 1) >= r->size)
        return 0;               /* the set does not describe this file */

    /* The set as a whole must rebuild the root we are downloading, or the
       leaves are not this file's and every segment check would be against
       a hash somebody else chose. */
    fold_root((const unsigned char *)pl->leaf, count, root);
    if (base32_decode(want, sizeof want, r->tth) != TTH_HASH_LEN)
        return 0;
    if (memcmp(root, want, TTH_HASH_LEN) != 0)
        return 0;

    /* Group the leaves into segments worth asking for on their own. A
       power of two so each group is a single node of the tree, and never
       so many groups that one source cannot be given a share. */
    pl->group = 1;
    while (pl->span * (tth_u64)pl->group < XQ_SEG_TARGET &&
           pl->group * 2 <= count)
        pl->group *= 2;
    pl->nseg = (count + pl->group - 1) / pl->group;

    /* Whatever is already right on disk does not need fetching again. This
       is the resume path, and it is per-segment: a partial file with a bad
       patch in the middle costs that patch, not the lot. */
    for (i = 0; i < pl->nseg; i++)
        if (segment_on_disk(r, pl, i))
            pl->done[i] = 1;

    pl->in_use = 1;
    snprintf(pl->tth, sizeof pl->tth, "%s", r->tth);
    r->plan = slot;
    return pl->nseg;
}

xq_plan *xq_plan_of(xfer_queue *q, const xq_request *r)
{
    if (!r || r->plan < 0 || r->plan >= XQ_MAX_DOWNLOADS)
        return NULL;
    if (!q->plans[r->plan] || !q->plans[r->plan]->in_use)
        return NULL;
    return q->plans[r->plan];
}

tth_u64 xq_plan_done_bytes(const xfer_queue *q, const xq_request *r)
{
    xq_plan *pl = xq_plan_of((xfer_queue *)q, r);
    tth_u64 total = 0;
    long i;

    if (!pl)
        return 0;

    for (i = 0; i < pl->nseg; i++)
        if (pl->done[i])
            total += segment_len(pl, i);
    return total;
}

int xq_next_segment(xfer_queue *q, xq_request *r, peerconn *p)
{
    xq_plan *pl = xq_plan_of(q, r);
    long i;

    if (!pl || !p)
        return 0;

    for (i = 0; i < pl->nseg; i++) {
        unsigned char want[TTH_HASH_LEN];

        if (pl->done[i] || pl->busy[i])
            continue;

        segment_hash(pl, i, want);

        if (peer_request_segment(p, i, segment_start(pl, i),
                                 segment_len(pl, i), want, r->local) != 0)
            return 0;

        pl->busy[i] = 1;
        return 1;
    }
    return 0;
}

int xq_assign_segments(xfer_queue *q)
{
    int handed = 0;
    int pass;

    /* Both arrays: a connection the peer dialled lives among the uploads
       (see xq_poll) and is just as able to fetch a part. */
    for (pass = 0; pass < 2; pass++) {
        peerconn *arr = pass ? q->up : q->down;
        const int *used = pass ? q->up_used : q->down_used;
        int n = pass ? XQ_MAX_UPLOADS : XQ_MAX_DOWNLOADS;
        int i;

        for (i = 0; i < n; i++) {
            peerconn *p;
            xq_request *r;

            if (!used[i])
                continue;

            p = &arr[i];
            if (p->uploading || p->state != PEER_IDLE)
                continue;

            r = xq_by_token(q, p->token);
            if (!r || !xq_plan_of(q, r))
                continue;

            /* Nothing to hand out leaves the connection idle rather than
               closed: the parts it could take are with other sources now,
               and if one of those fails this connection is already here. */
            if (xq_next_segment(q, r, p))
                handed++;
        }
    }

    return handed;
}

int xq_segment_done(xfer_queue *q, xq_request *r, long index)
{
    xq_plan *pl = xq_plan_of(q, r);
    long i;

    if (!pl || index < 0 || index >= pl->nseg)
        return 0;

    pl->done[index] = 1;
    pl->busy[index] = 0;

    for (i = 0; i < pl->nseg; i++)
        if (!pl->done[i])
            return 0;

    return 1;                   /* that was the last one */
}

void xq_segment_failed(xfer_queue *q, xq_request *r, long index)
{
    xq_plan *pl = xq_plan_of(q, r);

    if (!pl || index < 0 || index >= pl->nseg)
        return;

    /* Back in the pool for somebody else to fetch. Not marked done, so a
       bad segment from a bad source costs one segment. */
    pl->busy[index] = 0;
}

static void plan_release(xfer_queue *q, xq_request *r)
{
    xq_plan *pl = xq_plan_of(q, r);

    if (pl)
        pl->in_use = 0;
    r->plan = -1;
}

int xq_claim_token(xfer_queue *q, peerconn *p, const char *token)
{
    xq_request *r = xq_by_token(q, token);

    if (!r)
        return 0;               /* not ours */
    if (r->status != XQ_ASKING &&
        !(r->status == XQ_ACTIVE && xq_plan_of(q, r) != NULL))
        return 0;               /* not one we are waiting for */

    snprintf(p->token, sizeof p->token, "%s", r->token);
    snprintf(p->name, sizeof p->name, "%s", r->name);
    snprintf(p->local, sizeof p->local, "%s", r->local);
    if (r->wants_list) {
        p->want = PEER_WANT_LIST;
        /* Which directory, if this is a partial list. Setting the want and
           forgetting the base is how an inbound connection ended up asking
           for the whole share: peer_start_list fills this in for a
           connection we dialled, and nothing filled it in for one that
           dialled us. The AirDC++ rig is active, so that was the only path
           it ever took. */
        snprintf(p->list_base, sizeof p->list_base, "%s", r->list_base);
        p->tth[0] = '\0';
        p->size = 0;
        p->offset = 0;
    } else if (xq_wants_tthl(q, r)) {
        /* Leaves first here too: an inbound connection is just as good a
           place to ask from as one we dialled. */
        p->want = PEER_WANT_TTHL;
        snprintf(p->tth, sizeof p->tth, "%s", r->tth);
        snprintf(p->local, sizeof p->local, "%s", TTHL_FILE);
        p->size = 0;
        p->offset = 0;
        r->tthl_pending = 1;
    } else if (xq_plan_of(q, r) != NULL) {
        p->want = PEER_WANT_NOTHING;    /* a segment is assigned next */
        snprintf(p->tth, sizeof p->tth, "%s", r->tth);
        p->size = r->size;
        p->offset = 0;
    } else {
        p->want = PEER_WANT_FILE;
        snprintf(p->tth, sizeof p->tth, "%s", r->tth);
        p->size = r->size;
        p->offset = resume_point(r);
    }

    r->status = XQ_ACTIVE;
    return 1;
}

int xq_browse_pending(const xfer_queue *q)
{
    int i;

    for (i = 0; i < q->nreq; i++)
        if (q->reqs[i].wants_list &&
            (q->reqs[i].status == XQ_WAITING ||
             q->reqs[i].status == XQ_ASKING ||
             q->reqs[i].status == XQ_ACTIVE))
            return 1;
    return 0;
}

/* ----------------------------------------------- surviving a restart */

int xq_save(const xfer_queue *q, const char *path)
{
    FILE *f;
    int i, n = 0;

    /* Nothing worth keeping: remove the file rather than leaving a stale
       one that would re-queue finished downloads on the next start. */
    for (i = 0; i < q->nreq; i++) {
        const xq_request *r = &q->reqs[i];

        if (!r->wants_list && r->status != XQ_DONE &&
            r->status != XQ_FAILED)
            n++;
    }
    if (n == 0) {
        remove(path);
        return 0;
    }

    f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "; DOSy++ download queue -- rewritten as it changes.\n");
    fprintf(f, "; <tth> <size> <local path> <name>\n");

    for (i = 0; i < q->nreq; i++) {
        const xq_request *r = &q->reqs[i];

        if (r->wants_list || r->status == XQ_DONE ||
            r->status == XQ_FAILED)
            continue;

        /* The local path has no spaces (it is an 8.3 name we chose); the
           display name may, so it goes last and takes the rest of the
           line. */
        fprintf(f, "%s %lu %s %s\n", r->tth, (unsigned long)r->size,
                r->local, r->name);
    }

    if (fclose(f) != 0)
        return -1;
    return n;
}

int xq_load(xfer_queue *q, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0;

    if (!f)
        return 0;                   /* nothing queued last time */

    while (fgets(line, sizeof line, f)) {
        char tth[BASE32_TIGER_LEN + 16];
        char local[160];
        char name[XQ_NAME_MAX];
        unsigned long size = 0;
        tth_u64 got = 0;
        FILE *partial;

        if (line[0] == ';' || line[0] == '#' || line[0] == '\n')
            continue;

        if (sscanf(line, "%48s %lu %159s %95[^\n]",
                   tth, &size, local, name) < 3)
            continue;               /* a corrupt line is skipped, not fatal */

        /* How much is really there. Trusting a byte count from the file
           would mean resuming past the end of a partial that was truncated
           by whatever interrupted it. */
        partial = fopen(local, "rb");
        if (partial) {
            if (fseek(partial, 0, SEEK_END) == 0) {
                long at = ftell(partial);
                if (at > 0)
                    got = (tth_u64)at;
            }
            fclose(partial);
        }

        /* Already complete: nothing to resume, and re-queueing it would
           truncate a finished file. */
        if (size > 0 && got >= (tth_u64)size)
            continue;

        if (xq_add_saved(q, tth, (tth_u64)size, name, local, got))
            n++;
    }

    fclose(f);
    return n;
}

int xq_connect(xfer_queue *q, xq_request *r, const char *host, int port,
               const adc_identity *id, const peer_callbacks *cb,
               unsigned long now)
{
    transport t;
    int slot = free_down_slot(q);

    if (slot < 0)
        return -1;

    /* Blocking connect, briefly. A peer that does not answer promptly is
       not worth waiting on when there are other sources. */
    if (tp_plain_connect(&t, host, port, 10) != 0) {
        snprintf(r->err, sizeof r->err, "could not reach the peer");
        return -1;
    }

    if (start_for(q, slot, &t, r, id, cb, now) != 0) {
        tp_close(&t);
        return -1;
    }

    q->down_used[slot] = 1;
    r->status = XQ_ACTIVE;
    return 0;
}

int xq_adopt(xfer_queue *q, transport *t, const char *token, int outgoing,
             const adc_identity *id, share_index *share,
             const peer_callbacks *cb, unsigned long now)
{
    xq_request *r = xq_by_token(q, token);

    /* A connection for something we asked for. A segmented download is
       already ACTIVE when its second and third sources answer -- that is
       the point of it -- so "are we still waiting for a connection" is not
       simply "is the status ASKING". Getting this wrong turned the second
       source into a connection each side expected the other to ask on. */
    if (r && (r->status == XQ_ASKING ||
              (r->status == XQ_ACTIVE && xq_plan_of(q, r) != NULL))) {
        /* A peer dialling us back for something we asked for. */
        int slot = free_down_slot(q);

        if (slot < 0)
            return -1;

        if (start_for(q, slot, t, r, id, cb, now) != 0)
            return -1;

        q->down_used[slot] = 1;
        r->status = XQ_ACTIVE;
        return 0;
    }

    /* Otherwise it is someone wanting something from us. */
    {
        int slot = free_up_slot(q);
        int rc;

        if (slot < 0)
            return -1;              /* all slots busy: let it close */

        if (outgoing)
            rc = peer_start_serving(&q->up[slot], t, id, token, share, cb,
                                    now);
        else
            rc = peer_accept(&q->up[slot], t, id, share, cb, now);

        if (rc != 0)
            return -1;

        q->up[slot].no_serve_partial = q->no_serve_partial;

        q->up_used[slot] = 1;
        return 0;
    }
}

/* Mark the request belonging to a finished connection. */
static void settle(xfer_queue *q, peerconn *p)
{
    xq_request *r = xq_by_token(q, p->token);

    if (!r)
        return;

    /* Leaf hashes are a detour, not the download.
     *
     * Whether the peer refused them or sent something no plan could be
     * built from, the file is still there and still wanted -- and a peer
     * that says no to leaves is an ordinary source, not a bad one, so it
     * does not get struck off. What it costs is segmenting: the next
     * attempt fetches the whole file, which is what every client did
     * before segmenting existed.
     *
     * Both halves of that were broken, in opposite directions. A refusal
     * failed the connection with the request's only source already
     * counted, so it went to XQ_FAILED and the file could not be
     * downloaded at all. Unusable leaves closed the connection cleanly,
     * which the code below settles as a success -- the queue reported
     * DONE without a byte of the file arriving.
     *
     * Found with tools/stubhub.py --serve, which refuses tthl because
     * Python has no Tiger: the browser demo's own download failed. */
    if (p->want == PEER_WANT_TTHL && xq_plan_of(q, r) == NULL) {
        r->tthl_refused = 1;
        r->status = XQ_WAITING;
        snprintf(r->err, sizeof r->err,
                 "no leaf hashes -- fetching the whole file");
        return;
    }

    /* A segmented download is finished when its last segment verifies, not
       when a connection closes: connections come and go per segment. If
       parts are still missing the request goes back in the queue to be
       asked for again, from this source or another. */
    if (xq_plan_of(q, r) != NULL) {
        xq_plan *pl = xq_plan_of(q, r);
        long i;
        int missing = 0;

        for (i = 0; i < pl->nseg; i++) {
            if (!pl->done[i]) {
                missing = 1;
                pl->busy[i] = 0;        /* whoever had it is gone */
            }
        }

        if (!missing) {
            r->status = XQ_DONE;
            q->completed++;
            plan_release(q, r);
            return;
        }

        snprintf(r->err, sizeof r->err, "%s", peer_error(p));
        r->attempts++;
        if (r->attempts < 16) {
            r->status = XQ_WAITING;
        } else {
            r->status = XQ_FAILED;
            q->failed++;
            plan_release(q, r);
        }
        return;
    }

    if (p->state == PEER_DONE) {
        r->status = XQ_DONE;
        q->completed++;
        plan_release(q, r);     /* 24KB back for the next download */
    } else {
        /* Try the next source rather than giving up: the usual reason a
           transfer fails is the peer, not the file. */
        snprintf(r->err, sizeof r->err, "%s", peer_error(p));
        r->try_source++;
        r->attempts++;
        if (r->try_source < r->nsource && r->attempts < 8) {
            r->status = XQ_WAITING;
        } else {
            r->status = XQ_FAILED;
            q->failed++;
            plan_release(q, r);
        }
    }
}

int xq_poll(xfer_queue *q, unsigned long now)
{
    int progress = 0;
    int i;

    for (i = 0; i < XQ_MAX_DOWNLOADS; i++) {
        if (!q->down_used[i])
            continue;

        {
            peerconn *p = &q->down[i];
            tth_u64 before = p->moved;
            int rc = peer_poll(p, now);

            q->bytes_down += p->moved - before;

            if (rc > 0)
                progress = 1;
            if (rc < 0) {
                settle(q, p);
                /* The token is this connection's, not the request's, so it
                   dies with it -- otherwise the source would look busy for
                   ever and never be asked again. */
                xq_forget(q, p->token);
                peer_close(p);
                q->down_used[i] = 0;
                progress = 1;
            }
        }
    }

    for (i = 0; i < XQ_MAX_UPLOADS; i++) {
        if (!q->up_used[i])
            continue;

        {
            peerconn *p = &q->up[i];
            tth_u64 before = p->moved;
            int rc = peer_poll(p, now);

            /* A connection in this array is not necessarily an upload: one
               that arrived quoting a token of ours was claimed as the
               download we asked for (xq_claim_token), and it stays where it
               is rather than being shuffled between arrays. So the
               direction comes from the connection, and the request gets
               settled either way -- otherwise an active client's downloads
               would complete on the wire and never leave the queue. */
            if (p->uploading)
                q->bytes_up += p->moved - before;
            else
                q->bytes_down += p->moved - before;

            if (rc > 0)
                progress = 1;
            if (rc < 0) {
                if (!p->uploading)
                    settle(q, p);
                xq_forget(q, p->token);
                peer_close(p);
                q->up_used[i] = 0;
                progress = 1;
            }
        }
    }

    return progress;
}
