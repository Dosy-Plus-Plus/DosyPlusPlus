/* peerconn.h - client-to-client connections.
 *
 * A peer connection is a separate TCP session that speaks a subset of ADC:
 *
 *     A->B  CSUP ADBASE ADTIGR
 *     B->A  CSUP ADBASE ADTIGR
 *     A->B  CINF ID<cid> TO<token>        (the side that initiated)
 *     B->A  CINF ID<cid>
 *     A->B  CGET file TTH/<root> 0 -1
 *     B->A  CSND file TTH/<root> 0 <len>\n   then <len> raw bytes
 *
 * A file list is the same exchange with a different identifier:
 *
 *     A->B  CGET file files.xml.bz2 0 -1
 *     B->A  CSND file files.xml.bz2 0 <len>\n   then the bzip2 stream
 *
 * The list has no hash, so it is the one transfer that cannot be verified
 * by content. It is parsed defensively instead (share/xmlpull.c).
 *
 * Who connects to whom is decided over the hub:
 *
 *   - An ACTIVE downloader sends CTM: "connect to me at this port". The
 *     uploader dials out.
 *   - A PASSIVE downloader sends RCM: "I cannot be reached, send me a CTM".
 *     The uploader, if active, replies with CTM and the downloader dials.
 *   - Two passive clients can never connect. That is not a bug to work
 *     around; it is why being active matters.
 *
 * Files are addressed by hash (TTH/<root>), never by path. Any source with
 * the file can serve it, and the bytes are verified against that hash
 * before being accepted -- a transfer that completes with the wrong
 * content is worse than one that fails.
 *
 * One state machine serves both directions: whether this connection
 * uploads or downloads is decided by which side asked.
 */
#ifndef DOSYPP_PEERCONN_H
#define DOSYPP_PEERCONN_H

#include "../net/transport.h"
#include "../share/index.h"
#include "ident.h"
#include "tokenize.h"

typedef enum {
    PEER_SUP = 0,      /* exchanging CSUP */
    PEER_INF,          /* exchanging CINF */
    PEER_IDLE,         /* connected, nothing in flight */
    PEER_GETTING,      /* we asked; waiting for CSND */
    PEER_DOWNLOADING,  /* receiving raw bytes */
    PEER_SENDING,      /* serving raw bytes */
    PEER_DONE,         /* finished cleanly */
    PEER_FAILED
} peer_state;

/* One file-read's worth of upload, held between polls. An upload must not
   block: on a 486 the client is single-threaded, so spinning until the
   socket drains freezes the screen and the keyboard for the length of the
   transfer. The bytes that did not fit stay here and go out next poll. */
#define PEER_OUT_CHUNK 8192

#define PEER_ERR_MAX  160
#define PEER_PATH_MAX 200

/* What an outgoing connection is for.
 *
 * A file list and the leaf hashes are both "a blob with a name we were
 * given", and a segment is a byte range verified against one leaf rather
 * than the whole file against its root. Keeping them as one enum rather
 * than a pile of flags is the difference between reading this code and
 * guessing at it. */
typedef enum {
    PEER_WANT_NOTHING = 0,  /* we were dialled; wait to be asked */
    PEER_WANT_FILE,         /* the whole file, verified against its root */
    PEER_WANT_SEGMENT,      /* one byte range, verified against one leaf */
    PEER_WANT_LIST,         /* files.xml.bz2 */
    PEER_WANT_TTHL          /* the leaf hashes */
} peer_want;

typedef struct peerconn peerconn;

typedef struct {
    void *user;

    /* A download finished and its hash matched. `local` is where it was
       written. */
    void (*on_complete)(void *user, peerconn *p, const char *local);
    /* It failed; reason via peer_error(). */
    void (*on_failed)(void *user, peerconn *p);
    /* Progress, called as bytes move. */
    void (*on_progress)(void *user, peerconn *p);

    /* One segment arrived and matched its leaf hash. The connection is
       idle again and can be given another segment immediately, which is
       what makes a segmented download cheaper than a connection per
       part. */
    void (*on_segment)(void *user, peerconn *p, long index);

    /* An inbound connection presented a token in its CINF. That means it is
       answering a CTM we sent: the peer dialled us because we asked it to,
       and the token says which request it belongs to.
     *
       Return 1 having set `want` and filled in what goes with it (tth,
       name, size, local, offset) and this connection becomes that
       download.
       Return 0 and it stays what it looked like -- somebody wanting
       something from us.
     *
       Without this, an ACTIVE client can never download anything: it sends
       CTM, the peer dials back, and the connection sits there being treated
       as an upload request that never comes. Our own tests all had the
       downloader dialling out, so none of them noticed. AirDC++ did,
       immediately. */
    int (*on_token)(void *user, peerconn *p, const char *token);

    /* May we serve a file right now? Upload slots are the caller's policy,
       not this module's, so it asks. Returning 0 refuses the request with
       a status the peer can act on rather than a dropped connection.
       Not consulted for file lists: every client serves those, and a list
       that costs a slot makes browsing a hostile act. */
    int (*may_upload)(void *user, peerconn *p);
} peer_callbacks;

struct peerconn {
    transport      t;
    peer_state     state;
    int            incoming;         /* they dialled us */
    int            uploading;
    peer_want      want;            /* what we asked this peer for */

    /* For PEER_WANT_SEGMENT: which part, and the hash it must match. The
       hash of a byte range that covers exactly one node of the Merkle tree
       IS that node, so a segment is verified on its own, with no reference
       to the rest of the file and no final pass over it. */
    tth_u64        seg_start;
    tth_u64        seg_len;
    unsigned char  seg_hash[TTH_HASH_LEN];
    long           seg_index;        /* for the caller's bookkeeping */

    linebuf        in;
    adc_identity   id;               /* ours */
    char           peer_cid[ADC_CID_CHARS + 1];
    char           token[32];
    char           nick[32];         /* for display; filled by the caller */

    /* What is being transferred. */
    char           tth[BASE32_TIGER_LEN + 1];
    char           name[PEER_PATH_MAX];     /* display name */

    /* The directory a partial list was asked for, or "" for the whole
       share. Kept because the reply has to be matched against it: a peer
       that sends a list of somewhere else is not answering the question. */
    /* What we are serving, as it went out in the CSND: "file", "tthl" or
       "list". Kept because "an upload finished" is not useful without it --
       a file transfer and its leaf hashes are both uploads, and a rig that
       cannot tell them apart stops at the wrong one. */
    char           sending_type[8];

    char           list_base[PEER_PATH_MAX];
    int            list_refused;            /* the peer said no to it */
    /* Serving side: refuse partial lists and offer only the whole share.
       Exists so the fallback path can be tested against a real client
       rather than a mock, and as an escape hatch if a peer turns out to
       mishandle them. */
    int            no_serve_partial;
    char           local[PEER_PATH_MAX];    /* where we write or read */
    tth_u64        size;                    /* expected total */
    tth_u64        moved;                   /* bytes transferred */
    tth_u64        offset;                  /* start offset of this request */

    void          *fp;                      /* FILE*, cast to avoid stdio
                                               in the header */

    /* Upload staging: out[out_off..out_len) is read from the file and not
       yet accepted by the socket. */
    unsigned char  out[PEER_OUT_CHUNK];
    size_t         out_len;
    size_t         out_off;
    tth_ctx        verify;                  /* running hash of a download */
    int            verifying;

    share_index   *share;                   /* for uploads */
    peer_callbacks cb;

    unsigned long  started;                 /* seconds, for rate display */

    /* A one-second window, because a lifetime average is the wrong
       number and sometimes it is zero.
     *
     * The rate used to be moved/(now-started) in whole seconds, so a
     * connection showed 0 for its whole first second -- and a small file
     * on a fast link is finished inside that second, so its row read 0
     * from beginning to end. A lifetime average also keeps claiming a
     * rate a stalled transfer is no longer getting. */
    unsigned long  rate_mark_time;          /* seconds, window start */
    tth_u64        rate_mark_moved;         /* `moved` at that moment */
    unsigned long  rate_last;               /* bytes/s over the last window */
    unsigned long  last_activity;
    char           err[PEER_ERR_MAX];
};

/* An inbound connection: they dialled us and will identify themselves. */
int peer_accept(peerconn *p, transport *t, const adc_identity *id,
                share_index *share, const peer_callbacks *cb,
                unsigned long now);

/* An outbound connection we dialled because THEY asked us to -- their CTM,
   their token -- so that they can ask us for something.
 *
   This is not the same as accepting a connection, and conflating the two
   was a real bug: the token belongs to their request, and the side that
   dials is the side that must quote it. AirDC++ answers a CINF without a
   token with "CSTA 200 TO missing" and hangs up, which is correct of it. */
int peer_start_serving(peerconn *p, transport *t, const adc_identity *id,
                       const char *token, share_index *share,
                       const peer_callbacks *cb, unsigned long now);

/* An outbound connection we dialled, to fetch the peer's file list. The
   list is written to local_path as the raw bzip2 stream; nothing here
   decompresses or parses it. */
/* Fetch a file list. `base` is NULL for the whole share; "/" for the root
   one level deep; or a virtual directory ("/warez/apps/") for a partial
   list of that directory alone -- which is what makes browsing a 200,000-file share possible on a
   machine that cannot hold one. A partial list is asked for
   non-recursively: the point is one directory at a time.

   A peer that does not do partial lists answers with a CSTA, and the
   failure reason says so (peer_list_refused) so the caller can fall back
   to the whole list rather than giving up. */
int peer_start_list(peerconn *p, transport *t, const adc_identity *id,
                    const char *token, const char *local_path,
                    const char *base,
                    const peer_callbacks *cb, unsigned long now);

/* Did this connection fail because the peer refused a partial list, as
   opposed to failing for any of the ordinary reasons? Only meaningful
   after a failure on a connection that asked for one. */
int peer_list_refused(const peerconn *p);

/* Fetch the leaf hashes for a file, written raw to local_path: 24 bytes per
   node, in file order. What the level is, and therefore how many bytes each
   node covers, is worked out from the count and the file size -- ADC does
   not state it. */
int peer_start_tthl(peerconn *p, transport *t, const adc_identity *id,
                    const char *token, const char *tth,
                    const char *local_path,
                    const peer_callbacks *cb, unsigned long now);

/* Ask this connection for one segment, verified against `hash`. Valid on a
   connection that is PEER_IDLE -- which is how a segmented download reuses
   one connection for segment after segment instead of reconnecting. */
int peer_request_segment(peerconn *p, long index, tth_u64 start,
                         tth_u64 len, const unsigned char *hash,
                         const char *local_path);

/* An outbound connection we dialled, to fetch one file by hash. */
int peer_start_download(peerconn *p, transport *t, const adc_identity *id,
                        const char *token, const char *tth,
                        const char *display_name, tth_u64 size,
                        const char *local_path,
                        const peer_callbacks *cb, unsigned long now);

/* Read and dispatch. Returns 1 if progress was made, 0 if idle, -1 if the
   connection is finished (completed or failed -- check the state). */
int peer_poll(peerconn *p, unsigned long now);

void peer_close(peerconn *p);

/* Log every peer-protocol line to stdout. Process-wide, because the trace
   is a debugging aid and threading a flag through every call site to get it
   would be worse than a file-scope switch. */
void peer_trace(int on);

/* Is tracing on? So callers can log their own side of a decision in the
   same run without a second flag. */
int peer_trace_enabled(void);

const char *peer_error(const peerconn *p);
const char *peer_state_name(peer_state s);

/* Percent complete, integer, for the gauge. */
int peer_percent(const peerconn *p);

/* Bytes per second over the last second or so.
 *
 * Inside the first second it is the bytes moved so far, which is a figure
 * for a second that has not finished yet -- an estimate, and a far better
 * one than the zero this used to return. */
unsigned long peer_rate(const peerconn *p, unsigned long now);

#endif /* DOSYPP_PEERCONN_H */
