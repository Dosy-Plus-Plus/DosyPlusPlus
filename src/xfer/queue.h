/* queue.h - what we are downloading, and what we are serving.
 *
 * Deliberately small: a fixed number of slots, each holding one peer
 * connection and the request it belongs to. A DOS machine on a 14.4k modem
 * is not running thirty transfers, and a growable table here would buy
 * nothing but a way to run out of memory.
 *
 * The queue owns the connection setup dance, which is the fiddly part:
 *
 *   1. The user picks a result. We know its hash, size, and which SIDs have
 *      it -- not an address.
 *   2. We ask over the hub: CTM if we are active ("dial me"), RCM if not
 *      ("dial me is impossible, you dial me").
 *   3. A connection appears -- either we dialled out, or they did -- and is
 *      matched to the request by its token.
 *   4. Transfer, verify by hash, rename into place.
 *
 * A request that gets no connection is retried against its next source,
 * because the usual reason is that the peer is passive too, or gone.
 */
#ifndef DOSYPP_XFER_QUEUE_H
#define DOSYPP_XFER_QUEUE_H

#include "../adc/peerconn.h"
#include "../adc/search.h"
#include "../share/namemap.h"

/* Where the queue is kept between runs. 8.3, like everything else on a DOS
   disk. */
#define XQ_QUEUE_FILE "DOSYPP.QUE"

#define XQ_MAX_DOWNLOADS 4     /* at once; more is pointless on a modem */

/* Segments per file. 1024 is also the cap share_write_tthl works to, so a
   plan always fits whatever the file's size: the leaves just cover more
   bytes each as the file grows. 1024 leaves is 24KB of hashes and 128
   bytes of bookkeeping. */
#define XQ_MAX_SEGS      1024
#define XQ_MAX_UPLOADS   4
#define XQ_MAX_QUEUED    64
#define XQ_TOKEN_MAX     24
#define XQ_NAME_MAX      96

typedef enum {
    XQ_WAITING = 0,    /* queued, no slot yet */
    XQ_ASKING,         /* CTM/RCM sent, waiting for a connection */
    XQ_ACTIVE,         /* transferring */
    XQ_DONE,
    XQ_FAILED
} xq_status;

typedef struct {
    char        name[XQ_NAME_MAX];      /* real name, for display */
    char        tth[BASE32_TIGER_LEN + 1];
    tth_u64     size;

    adc_sid     sources[SR_MAX_SOURCES];
    int         nsource;
    int         try_source;             /* next one to attempt */

    /* Which plan in the queue's table this request is using, or -1 for a
       whole-file download. */
    int         plan;

    /* We asked a source for leaves and are waiting for them. */
    int         tthl_pending;
    int         tthl_refused;   /* nobody would give us any: fall back */

    char        token[XQ_TOKEN_MAX];
    char        local[128];             /* the 8.3 name we write to */

    /* A file-list fetch rather than a file. It goes through the same queue
       because everything fiddly about it -- asking over the hub, matching
       the connection by token, timing out, trying the next source -- is
       identical, and duplicating that for browse would mean two of every
       bug. The differences are that there is no hash to verify and the
       size is unknown until the peer says. */
    int         wants_list;
    /* Which directory, for a partial list. Empty means the whole share.
       A list request is identified by node *and* base: browsing two
       directories of the same share in a session is two requests. */
    char        list_base[128];

    /* Resumed from a previous run: how many bytes were already on disk.
       Kept because the queue file records it and the request has to carry
       it to the connection. */
    tth_u64     resume_from;
    xq_status   status;
    unsigned long asked_at;
    int         attempts;
    char        err[96];
} xq_request;

/* A segmented download in progress.
 *
 * Only live downloads have one of these -- at most XQ_MAX_DOWNLOADS, and
 * 24KB each -- because a plan for every queued request would be 24MB.
 *
 * The leaves are checked as a set against the root we advertise before any
 * of them is trusted (xq_plan_verify). After that, each segment stands on
 * its own: the hash of a byte range covering one node IS that node, so a
 * segment is verified when it lands and the finished file needs no pass
 * over it at all. That is what makes resume cheap as well as parallel
 * downloads possible. */
typedef struct {
    int           in_use;
    char          tth[BASE32_TIGER_LEN + 1];
    tth_u64       size;

    unsigned char leaf[XQ_MAX_SEGS][TTH_HASH_LEN];
    long          nleaf;
    tth_u64       span;                 /* bytes each leaf covers */

    /* Leaves per segment: a power of two, so a segment is exactly one node
       of the tree and its hash is the fold of those leaves. One leaf per
       segment would mean a request per kilobyte -- which worked, and was
       absurd to watch. */
    long          group;
    long          nseg;

    unsigned char done[XQ_MAX_SEGS];    /* 1 = verified and on disk */
    unsigned char busy[XQ_MAX_SEGS];    /* 1 = some connection has it */
} xq_plan;

/* Bytes to aim for in one segment. Big enough that the per-request round
   trip disappears into it, small enough that a dead source costs little
   and that several sources get a fair share of a modest file. */
#define XQ_SEG_TARGET 65536

/* One attempted connection: the token we minted for it, and what it is
 * for.
 *
 * A token per connection, not per request, is what lets a segmented
 * download run against several sources at once -- two connections for one
 * file cannot share a token, because the token is how an arriving
 * connection says which request it belongs to. It also keeps the "one
 * connection per peer" rule honest: that is a property of connections, and
 * before this there was nowhere to record it.
 */
typedef struct {
    int      live;
    char     token[XQ_TOKEN_MAX];
    int      req;                /* index into reqs[] */
    adc_sid  src;                /* who we asked */
    unsigned long asked_at;
} xq_conn;

#define XQ_MAX_CONNS (XQ_MAX_DOWNLOADS + XQ_MAX_UPLOADS)

typedef struct {
    xq_request   reqs[XQ_MAX_QUEUED];
    int          nreq;

    xq_conn      conns[XQ_MAX_CONNS];

    peerconn     down[XQ_MAX_DOWNLOADS];
    int          down_used[XQ_MAX_DOWNLOADS];
    peerconn     up[XQ_MAX_UPLOADS];
    int          up_used[XQ_MAX_UPLOADS];

    namemap     *names;
    unsigned long token_seq;

    /* Plans, one per segmented download in flight. Heap-allocated so an
       idle client does not carry 96KB it is not using. */
    xq_plan     *plans[XQ_MAX_DOWNLOADS];

    /* Serve only whole file lists. A compatibility and test switch rather
       than a setting -- see --refuse-partial -- which is how the asking
       side's fallback gets tested against a real client instead of a
       mock. */
    int          no_serve_partial;

    /* Totals for the door. */
    unsigned int completed;
    unsigned int failed;
    tth_u64      bytes_down;
    tth_u64      bytes_up;
} xfer_queue;

int  xq_init(xfer_queue *q, namemap *names);
void xq_free(xfer_queue *q);

/* Queue a search result. Returns the request, or NULL if the queue is full
   or the file is already queued. */
xq_request *xq_add(xfer_queue *q, const search_result *r,
                   const char *download_dir);

/* Queue a file-list fetch from one node. `nick` is for display only.
   `base` is NULL for the whole share, "/" for the root one level deep, or
   a virtual directory for a partial list of it alone. Returns the request, or NULL if the queue is
   full. A second request for the same node and base replaces the first
   rather than queueing twice -- a user who presses ENTER on a directory
   twice should wait, not fetch it twice. */
xq_request *xq_add_list(xfer_queue *q, adc_sid src, const char *nick,
                        const char *local_path, const char *base);

/* Queue something recovered from the queue file: a hash, a size and a
   local path, with no sources yet. The request waits for a TR search to
   find somebody who has it, which is what xq_add_source is for.

   `got` is how much of the file is already on disk. */
xq_request *xq_add_saved(xfer_queue *q, const char *tth, tth_u64 size,
                         const char *name, const char *local, tth_u64 got);

/* Attach a source to a request that is waiting for one. Returns 1 if it
   was added (or was already there), 0 if the request has no room. */
int xq_add_source(xfer_queue *q, xq_request *r, adc_sid sid);

/* The request waiting for sources with this hash, or NULL. Used to route a
   search result back to a resumed download. */
xq_request *xq_by_tth(xfer_queue *q, const char *tth);

/* An inbound connection presented a token. If it matches a request we are
   waiting for a connection for, fill `p` in as that download and return 1;
   otherwise return 0 and it stays an upload request.
 *
   This is what makes active mode able to download at all: we send CTM, the
   peer dials back, and the only thing tying that connection to what we
   asked for is the token it quotes. */
int xq_claim_token(xfer_queue *q, peerconn *p, const char *token);

/* Is a file-list fetch in flight? The browse door asks, so it can say
   "fetching" rather than "nobody's files are open". */
int xq_browse_pending(const xfer_queue *q);

/* --- surviving a restart ---------------------------------------------
 *
 * A download interrupted by a power cut, a crash or a quit is worth
 * resuming: over a modem the alternative is paying for those megabytes
 * twice. Only unfinished file requests are saved -- not file lists, which
 * are cheap to ask for again, and not completed or failed ones.
 *
 * The file records the hash, the expected size, the display name and the
 * local path. Sources are deliberately not saved: a SID is meaningless in
 * the next session, so a resumed request goes looking for whoever has the
 * hash now. */
int xq_save(const xfer_queue *q, const char *path);

/* Load what a previous run left. Each entry's resume point is taken from
   the partial file on disk, not from the file we wrote, because the disk is
   the thing that is actually true. Returns the number of requests
   restored. */
int xq_load(xfer_queue *q, const char *path);

/* Slots. */
int xq_downloads_active(const xfer_queue *q);
int xq_uploads_active(const xfer_queue *q);

/* Uploads currently sending, which is what an upload slot limit counts:
   an idle connection is not using a slot. */
int xq_uploads_sending(const xfer_queue *q);
int xq_free_upload_slots(const xfer_queue *q, int configured);

/* The next request that needs a connection asked for, or NULL. `*src` gets
   the source to ask -- which is not simply the request's cursor any more: a
   segmented download with three sources opens three connections, so this
   returns the next source that has no connection of ours already. */
xq_request *xq_next_to_ask(xfer_queue *q, adc_sid *src);

/* Record that we asked `src` for `r` with this token, so the connection can
   be matched when it appears and the source is not asked twice. Returns 0,
   or -1 if there is no room (which means we are already as parallel as the
   queue allows). */
int xq_note_asked(xfer_queue *q, xq_request *r, adc_sid src,
                  const char *token, unsigned long now);

/* Forget a connection attempt, by token. */
void xq_forget(xfer_queue *q, const char *token);

/* Attempts that nobody answered, so the source can be tried again.
   Returns how many were given up on. */
int xq_expire_asks(xfer_queue *q, unsigned long now, unsigned long secs);

/* Find a request by the token a peer presented. This is how an inbound
   connection is matched to what we asked for. */
xq_request *xq_by_token(xfer_queue *q, const char *token);

/* Give a connected transport to the queue.
 *
 * `token` matches it to a request we made. An unmatched connection is one
 * where the peer wants something from us.
 *
 * `outgoing` says who dialled, and it matters more than it looks: the side
 * that dialled must quote the token in its CINF. A connection we made in
 * answer to somebody's CTM carries THEIR token, and sending our INF without
 * it gets us "CSTA 200 TO missing" and a closed socket from any client that
 * checks -- AirDC++ does.
 *
 * Returns 0 if adopted. */
int xq_adopt(xfer_queue *q, transport *t, const char *token, int outgoing,
             const adc_identity *id, share_index *share,
             const peer_callbacks *cb, unsigned long now);

/* Start an outbound download connection for a request. */
int xq_connect(xfer_queue *q, xq_request *r, const char *host, int port,
               const adc_identity *id, const peer_callbacks *cb,
               unsigned long now);

/* Poll every live connection. Returns the number that made progress. */
int xq_poll(xfer_queue *q, unsigned long now);

/* --- segmented downloads ---------------------------------------------
 *
 * The sequence, which the app drives:
 *
 *   1. A request with more than one source, or any source at all if we want
 *      resume to be cheap, asks a peer for `tthl` (xq_wants_tthl).
 *   2. The leaves arrive in a file; xq_plan_load reads them, works out what
 *      each covers, checks the set against the root, and marks whatever is
 *      already correct on disk as done.
 *   3. xq_next_segment hands out the parts, one per idle connection, and
 *      xq_segment_done marks them off as they verify.
 *
 * A source that refuses `tthl` is not a problem: the request falls back to
 * the whole-file path, which is what every DOSy++ before this did. */

/* Should this request fetch leaf hashes before transferring? */
int xq_wants_tthl(const xfer_queue *q, const xq_request *r);

/* Read leaves from `path` for a request and build its plan. Returns the
   number of segments, 0 if the leaves are unusable (in which case the
   request falls back to a whole-file download), or -1 on error. */
long xq_plan_load(xfer_queue *q, xq_request *r, const char *path);

/* The plan for a request, or NULL if it has none. */
xq_plan *xq_plan_of(xfer_queue *q, const xq_request *r);

/* How much of a segmented request is verified, for the gauge. */
tth_u64 xq_plan_done_bytes(const xfer_queue *q, const xq_request *r);

/* Give an idle connection its next segment. Returns 1 if one was handed
   out, 0 if there is nothing left to ask for. */
int xq_next_segment(xfer_queue *q, xq_request *r, peerconn *p);

/* Give every idle segmented connection its next part. Called each tick:
   connections become free as segments verify, and a connection that was
   waiting because everything was taken can pick up a part that another
   source dropped. Returns how many were handed out. */
int xq_assign_segments(xfer_queue *q);

/* A segment landed and matched. Returns 1 when that was the last one. */
int xq_segment_done(xfer_queue *q, xq_request *r, long index);

/* Hand a segment back: the connection failed or the peer refused it. */
void xq_segment_failed(xfer_queue *q, xq_request *r, long index);

/* A fresh token, unique within this session. */
void xq_mint_token(xfer_queue *q, char *out, size_t outsz);

const char *xq_status_name(xq_status s);

#endif /* DOSYPP_XFER_QUEUE_H */
