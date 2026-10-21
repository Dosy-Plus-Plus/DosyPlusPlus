/* peerconn.c - the client-to-client state machine, both directions. */
#include "peerconn.h"
#include "../share/filelist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What we support, client to client. BZIP says we handle bzip2-compressed
   file lists -- we read them and write them -- and some clients look for it
   before offering one. Not ADC0: that is encrypted client-to-client, which
   we do not have. */
#define PEER_FEATURES "ADBASE ADTIGR ADBZIP"
#define XFER_CHUNK    PEER_OUT_CHUNK

/* Optional protocol trace. A peer connection is the one part of the client
   with no hub log to consult: when a handshake goes wrong, nothing anywhere
   records what crossed the wire, and "the peer closed" is all you get. Off
   unless --peer-trace asks for it. */
static int trace_on = 0;

void peer_trace(int on)
{
    trace_on = on ? 1 : 0;
}

int peer_trace_enabled(void)
{
    return trace_on;
}

static void tr(const peerconn *p, const char *dir, const char *line)
{
    if (!trace_on)
        return;

    printf("peer %s %s %s\n", p->incoming ? "<in>" : "<out>", dir, line);

    /* Flushed, because a trace nobody can read until the program exits is
       not a trace. On DOS with output redirected to a device, stdout is
       fully buffered: the DOSBox rigs read their log after the run and
       never noticed, but the v86 harness watches COM1 live and saw
       nothing at all for a session that was working perfectly. */
    fflush(stdout);
}

static int request_file(peerconn *p);

static void fail(peerconn *p, const char *why)
{
    snprintf(p->err, sizeof p->err, "%s", why);
    tr(p, "!!!", why);
    p->state = PEER_FAILED;
    if (p->cb.on_failed)
        p->cb.on_failed(p->cb.user, p);
}

static int send_line(peerconn *p, const char *line)
{
    char buf[ADC_LINE_MAX];
    size_t n = strlen(line);

    if (n + 2 > sizeof buf) {
        fail(p, "outgoing line too long");
        return -1;
    }
    memcpy(buf, line, n);
    buf[n] = '\n';
    buf[n + 1] = '\0';

    tr(p, "-->", line);

    if (tp_write_all(&p->t, buf, n + 1) != 0) {
        fail(p, "write failed");
        return -1;
    }
    return 0;
}

const char *peer_error(const peerconn *p)
{
    return (p && p->err[0]) ? p->err : "no error";
}

const char *peer_state_name(peer_state s)
{
    switch (s) {
    case PEER_SUP:         return "HANDSHAKE";
    case PEER_INF:         return "IDENTIFY";
    case PEER_IDLE:        return "IDLE";
    case PEER_GETTING:     return "REQUESTING";
    case PEER_DOWNLOADING: return "RECEIVING";
    case PEER_SENDING:     return "SENDING";
    case PEER_DONE:        return "COMPLETE";
    default:               return "FAILED";
    }
}

int peer_percent(const peerconn *p)
{
    tth_u64 total, moved;

    if (!p || p->size == 0)
        return 0;

    total = p->size;
    moved = p->moved;

    /* Integer only, and scaled down first so a large file cannot overflow
       the multiply. */
    while (total > 0xFFFFFFUL) {
        total >>= 4;
        moved >>= 4;
    }
    if (total == 0)
        return 100;
    return (int)((moved * 100) / total);
}

/* Close the current window if a second has gone by, and open the next.
   Called from peer_poll, which is the one place that has both the clock
   and every byte that has moved. */
static void rate_tick(peerconn *p, unsigned long now)
{
    if (p->rate_mark_time == 0) {
        p->rate_mark_time = now;
        p->rate_mark_moved = p->moved;
        return;
    }

    if (now > p->rate_mark_time) {
        unsigned long secs = now - p->rate_mark_time;
        tth_u64 bytes = (p->moved >= p->rate_mark_moved)
                        ? p->moved - p->rate_mark_moved : 0;

        p->rate_last = (unsigned long)(bytes / secs);
        p->rate_mark_time = now;
        p->rate_mark_moved = p->moved;
    }
}

unsigned long peer_rate(const peerconn *p, unsigned long now)
{
    if (!p)
        return 0;

    /* A window has closed: that is the rate. */
    if (p->rate_last)
        return p->rate_last;

    /* It has not, so this is the first second (or the one after a stall).
       The bytes moved since the window opened are the best estimate
       available, and they are not zero while anything is arriving --
       which is the whole complaint this answers: a small file finished
       inside one second and its row read 0 for the entire transfer. */
    if (p->moved > p->rate_mark_moved)
        return (unsigned long)(p->moved - p->rate_mark_moved);

    (void)now;
    return 0;
}

/* --------------------------------------------------------------- setup */

static void common_init(peerconn *p, transport *t, const adc_identity *id,
                        const peer_callbacks *cb, unsigned long now)
{
    memset(p, 0, sizeof *p);
    p->t = *t;
    p->id = *id;
    if (cb)
        p->cb = *cb;
    linebuf_init(&p->in);
    p->started = now;
    p->last_activity = now;
}

int peer_accept(peerconn *p, transport *t, const adc_identity *id,
                share_index *share, const peer_callbacks *cb,
                unsigned long now)
{
    common_init(p, t, id, cb, now);
    p->incoming = 1;
    p->share = share;
    p->state = PEER_SUP;

    /* Both sides send CSUP immediately; there is no "server speaks first"
       rule to wait on. */
    return send_line(p, "CSUP " PEER_FEATURES);
}

int peer_start_serving(peerconn *p, transport *t, const adc_identity *id,
                       const char *token, share_index *share,
                       const peer_callbacks *cb, unsigned long now)
{
    common_init(p, t, id, cb, now);

    /* We dialled, so we identify the connection -- with their token, which
       is the only thing tying it to the request they made. */
    p->incoming = 0;
    p->uploading = 1;
    p->want = PEER_WANT_NOTHING;
    p->share = share;
    p->state = PEER_SUP;
    snprintf(p->token, sizeof p->token, "%s", token ? token : "");

    return send_line(p, "CSUP " PEER_FEATURES);
}

int peer_start_list(peerconn *p, transport *t, const adc_identity *id,
                    const char *token, const char *local_path,
                    const char *base,
                    const peer_callbacks *cb, unsigned long now)
{
    common_init(p, t, id, cb, now);
    p->incoming = 0;
    p->uploading = 0;
    p->want = PEER_WANT_LIST;
    p->state = PEER_SUP;

    /* NULL means the whole share, asked for by the name every client
       understands. "/" is NOT that: it means the root, one level deep,
       which is where browsing a share too big to hold starts. Conflating
       the two was good for exactly one afternoon. */
    if (base && base[0])
        snprintf(p->list_base, sizeof p->list_base, "%s", base);
    else
        p->list_base[0] = '\0';

    snprintf(p->token, sizeof p->token, "%s", token ? token : "");
    snprintf(p->name, sizeof p->name, "%s",
             p->list_base[0] ? p->list_base : "file list");
    snprintf(p->local, sizeof p->local, "%s", local_path ? local_path : "");

    /* Unknown until the CSND says so; a list has no advertised size the way
       a search result does. */
    p->size = 0;

    return send_line(p, "CSUP " PEER_FEATURES);
}

int peer_start_tthl(peerconn *p, transport *t, const adc_identity *id,
                    const char *token, const char *tth,
                    const char *local_path,
                    const peer_callbacks *cb, unsigned long now)
{
    common_init(p, t, id, cb, now);
    p->incoming = 0;
    p->uploading = 0;
    p->want = PEER_WANT_TTHL;
    p->state = PEER_SUP;

    snprintf(p->token, sizeof p->token, "%s", token ? token : "");
    snprintf(p->tth, sizeof p->tth, "%s", tth ? tth : "");
    snprintf(p->name, sizeof p->name, "%s", "leaf hashes");
    snprintf(p->local, sizeof p->local, "%s", local_path ? local_path : "");
    p->size = 0;                /* the CSND says how many there are */

    return send_line(p, "CSUP " PEER_FEATURES);
}

int peer_request_segment(peerconn *p, long index, tth_u64 start,
                         tth_u64 len, const unsigned char *hash,
                         const char *local_path)
{
    if (p->state != PEER_IDLE) {
        fail(p, "asked for a segment on a connection that is not idle");
        return -1;
    }
    if (!hash || len == 0)
        return -1;

    p->want = PEER_WANT_SEGMENT;
    p->uploading = 0;
    p->seg_index = index;
    p->seg_start = start;
    p->seg_len = len;
    memcpy(p->seg_hash, hash, TTH_HASH_LEN);

    if (local_path && local_path[0])
        snprintf(p->local, sizeof p->local, "%s", local_path);

    /* The connection may have moved bytes already, for a previous segment:
       reset the counters so the gauge and the completion test are about
       this segment. */
    p->offset = start;
    p->size = start + len;
    p->moved = start;

    return request_file(p);
}

int peer_start_download(peerconn *p, transport *t, const adc_identity *id,
                        const char *token, const char *tth,
                        const char *display_name, tth_u64 size,
                        const char *local_path,
                        const peer_callbacks *cb, unsigned long now)
{
    common_init(p, t, id, cb, now);
    p->incoming = 0;
    p->uploading = 0;
    p->want = PEER_WANT_FILE;
    p->state = PEER_SUP;

    snprintf(p->token, sizeof p->token, "%s", token ? token : "");
    snprintf(p->tth, sizeof p->tth, "%s", tth ? tth : "");
    snprintf(p->name, sizeof p->name, "%s",
             display_name ? display_name : "(unnamed)");
    snprintf(p->local, sizeof p->local, "%s", local_path ? local_path : "");
    p->size = size;

    return send_line(p, "CSUP " PEER_FEATURES);
}

void peer_close(peerconn *p)
{
    if (p->fp) {
        fclose((FILE *)p->fp);
        p->fp = NULL;
    }
    tp_close(&p->t);
}

/* ------------------------------------------------------- the handshake */

static int send_our_inf(peerconn *p)
{
    char line[ADC_LINE_MAX];

    /* The token identifies which request this connection belongs to, and
       only the side that initiated includes it. */
    if (p->token[0])
        snprintf(line, sizeof line, "CINF ID%s TO%s",
                 p->id.cid_b32, p->token);
    else
        snprintf(line, sizeof line, "CINF ID%s", p->id.cid_b32);

    return send_line(p, line);
}

static int request_file(peerconn *p)
{
    char line[ADC_LINE_MAX];

    switch (p->want) {
    case PEER_WANT_LIST:
        if (p->list_base[0]) {
            /* One directory. No RE1: recursive would be the whole subtree,
               which for the top of somebody's share is the whole list
               again -- the thing partial lists exist to avoid. */
            snprintf(line, sizeof line, "CGET list %s 0 -1", p->list_base);
            break;
        }
        /* The identifier every client understands for a full file list.
           Asking for `list /` instead would be the newer form, but it is
           the one some clients answer only partially. */
        snprintf(line, sizeof line, "CGET file %s 0 -1",
                 FILELIST_WIRE_NAME);
        break;

    case PEER_WANT_TTHL:
        if (!p->tth[0]) {
            fail(p, "no hash to fetch leaves for");
            return -1;
        }
        snprintf(line, sizeof line, "CGET tthl TTH/%s 0 -1", p->tth);
        break;

    case PEER_WANT_SEGMENT:
        if (!p->tth[0]) {
            fail(p, "no hash to fetch a segment of");
            return -1;
        }
        /* An explicit length, unlike the whole-file case: this is one part
           of the file and the peer must not send past the end of it. */
        snprintf(line, sizeof line, "CGET file TTH/%s %lu %lu",
                 p->tth, (unsigned long)p->seg_start,
                 (unsigned long)p->seg_len);
        break;

    case PEER_WANT_FILE:
        if (!p->tth[0]) {
            fail(p, "nothing to request");
            return -1;
        }
        /* By hash, not by path: any source holding the file can serve it,
           and the path they happen to keep it under is irrelevant. -1
           means "to the end". */
        snprintf(line, sizeof line, "CGET file TTH/%s %lu -1",
                 p->tth, (unsigned long)p->offset);
        break;

    default:
        fail(p, "nothing to request");
        return -1;
    }

    p->state = PEER_GETTING;
    return send_line(p, line);
}

/* --------------------------------------------------------- downloading */

static void finish_receive(peerconn *p);

/* Read what a previous run already downloaded into the running hash, so a
   resumed transfer can still be verified at the end. Returns 0, or -1
   having failed the connection. */
static int hash_existing(peerconn *p)
{
    unsigned char buf[XFER_CHUNK];
    tth_u64 left = p->offset;

    if (fseek((FILE *)p->fp, 0, SEEK_SET) != 0) {
        fail(p, "cannot re-read the partial file to verify it");
        return -1;
    }

    while (left > 0) {
        size_t want = sizeof buf;
        size_t got;

        if ((tth_u64)want > left)
            want = (size_t)left;

        got = fread(buf, 1, want, (FILE *)p->fp);
        if (got == 0) {
            /* The file shrank since the queue recorded its length. Treat it
               as unusable rather than guessing. */
            fail(p, "the partial file is shorter than the queue says");
            return -1;
        }
        tth_update(&p->verify, buf, got);
        left -= (tth_u64)got;
    }

    if (fseek((FILE *)p->fp, (long)p->offset, SEEK_SET) != 0) {
        fail(p, "cannot seek back after verifying the partial file");
        return -1;
    }
    return 0;
}

static int begin_receive(peerconn *p, tth_u64 declared)
{
    /* A segment lands in the middle of a file that may not exist yet, so it
       cannot use "wb" (which would throw away the other segments) or demand
       that the file already be long enough. */
    if (p->want == PEER_WANT_SEGMENT) {
        p->fp = fopen(p->local, "r+b");
        if (!p->fp)
            p->fp = fopen(p->local, "wb");
    } else {
        p->fp = fopen(p->local, p->offset ? "r+b" : "wb");
    }

    if (!p->fp) {
        fail(p, "cannot open the local file for writing");
        return -1;
    }

    if (p->offset &&
        fseek((FILE *)p->fp, (long)p->offset, SEEK_SET) != 0) {
        fail(p, "cannot seek to resume");
        return -1;
    }

    /* Verify as the bytes arrive rather than re-reading the file
       afterwards: on a 486 a second pass costs as much as the download.
       A file list is the exception -- it has no hash to check against, and
       neither do leaf hashes, which are checked as a set against the root
       once they are all in. */
    p->verifying = (p->want == PEER_WANT_FILE ||
                    p->want == PEER_WANT_SEGMENT);
    if (p->verifying) {
        tth_init(&p->verify, NULL, NULL, -1);

        /* A segment is its own tree: the hash of a byte range covering one
           node IS that node, so nothing before it needs hashing. Only the
           whole-file case has a prefix to account for.
         *
         * That prefix read is a blocking pass over everything downloaded so
         * far -- about two seconds per megabyte on a 486. It is still much
         * cheaper than fetching those megabytes again over a modem, and the
         * alternative is accepting a resumed file we never verified, which
         * defeats the point of addressing files by hash. Segmented
         * downloads avoid it entirely, which is half of why they are worth
         * having. */
        if (p->want == PEER_WANT_FILE && p->offset > 0 &&
            hash_existing(p) != 0)
            return -1;
    }

    if (p->want == PEER_WANT_SEGMENT) {
        /* A peer that offers a different amount than we asked for is not
           one to argue with: taking it would leave a hole or an overlap,
           and both are silent corruption. */
        if (declared != p->seg_len) {
            char msg[PEER_ERR_MAX];

            snprintf(msg, sizeof msg,
                     "asked for %lu bytes at %lu and was offered %lu",
                     (unsigned long)p->seg_len,
                     (unsigned long)p->seg_start,
                     (unsigned long)declared);
            fail(p, msg);
            return -1;
        }
    }

    p->size = declared + p->offset;
    p->moved = p->offset;
    p->state = PEER_DOWNLOADING;

    /* The peer almost always sends CSND and the first chunk of data in one
       TCP segment, so the line framer has already swallowed the start of
       the file. Take it back before reading any more from the socket --
       without this every transfer loses its first few kilobytes and fails
       verification, which looks like a hashing bug. */
    {
        const char *left = NULL;
        size_t n = linebuf_pending(&p->in, &left);

        if (n > 0) {
            tth_u64 room = p->size - p->moved;

            if ((tth_u64)n > room)
                n = (size_t)room;

            if (n > 0) {
                if (fwrite(left, 1, n, (FILE *)p->fp) != n) {
                    fail(p, "disk full or write error");
                    return -1;
                }
                if (p->verifying)
                    tth_update(&p->verify, left, n);
                p->moved += (tth_u64)n;
                linebuf_consume(&p->in, n);
            }
        }
    }

    if (p->moved >= p->size) {
        finish_receive(p);
        return 0;
    }

    return 0;
}

static void finish_receive(peerconn *p)
{
    unsigned char root[TTH_HASH_LEN];
    char got[BASE32_TIGER_LEN + 1];

    fclose((FILE *)p->fp);
    p->fp = NULL;

    if (p->want == PEER_WANT_SEGMENT) {
        tth_final(&p->verify, root);

        if (memcmp(root, p->seg_hash, TTH_HASH_LEN) != 0) {
            char msg[PEER_ERR_MAX];

            /* Only this segment is wrong. The rest of the file is still
               good, which is the other half of why segments are worth
               having: one bad source costs one segment, not the download.
               The caller decides whether to ask somebody else. */
            snprintf(msg, sizeof msg,
                     "segment %ld (%lu bytes at %lu) failed its leaf hash",
                     p->seg_index, (unsigned long)p->seg_len,
                     (unsigned long)p->seg_start);
            fail(p, msg);
            return;
        }

        /* Done with this segment, and the connection is reusable: the
           caller hands it the next one with peer_request_segment. */
        p->state = PEER_IDLE;
        p->want = PEER_WANT_NOTHING;
        if (p->cb.on_segment)
            p->cb.on_segment(p->cb.user, p, p->seg_index);
        return;
    }

    if (p->verifying) {
        tth_final(&p->verify, root);
        base32_encode(got, sizeof got, root, TTH_HASH_LEN);

        /* The whole point of addressing by hash. A transfer that completes
           with the wrong bytes is worse than one that fails, because it
           gets shared on. */
        if (strcmp(got, p->tth) != 0) {
            char msg[PEER_ERR_MAX];
            snprintf(msg, sizeof msg,
                     "hash mismatch: got %s, expected %s", got, p->tth);
            remove(p->local);
            fail(p, msg);
            return;
        }
    }

    p->state = PEER_DONE;
    if (p->cb.on_complete)
        p->cb.on_complete(p->cb.user, p, p->local);
}

static int receive_bytes(peerconn *p, unsigned long now)
{
    unsigned char buf[XFER_CHUNK];
    int moved = 0;
    int guard;

    for (guard = 0; guard < 8; guard++) {
        tth_u64 left = p->size - p->moved;
        size_t want = sizeof buf;
        int n;

        if (left == 0)
            break;
        if ((tth_u64)want > left)
            want = (size_t)left;

        n = tp_read(&p->t, buf, want);
        if (n == TP_AGAIN)
            break;
        if (n == TP_CLOSED) {
            fail(p, "the peer closed mid-transfer");
            return -1;
        }
        if (n < 0) {
            fail(p, "read error during transfer");
            return -1;
        }

        if (fwrite(buf, 1, (size_t)n, (FILE *)p->fp) != (size_t)n) {
            fail(p, "disk full or write error");
            return -1;
        }
        if (p->verifying)
            tth_update(&p->verify, buf, (size_t)n);

        p->moved += (tth_u64)n;
        p->last_activity = now;
        moved = 1;

        if (p->moved >= p->size) {
            finish_receive(p);
            return 1;
        }
    }

    if (moved && p->cb.on_progress)
        p->cb.on_progress(p->cb.user, p);

    return moved;
}

/* ----------------------------------------------------------- uploading */

/* Serve a local file under a type and identifier of our choosing. The file
   list and the leaf hashes both use this: neither is in the share index,
   and neither must be looked up there. */
static int begin_send_path(peerconn *p, const char *local,
                           const char *wire_type, const char *wire_ident,
                           const char *display,
                           tth_u64 offset, long long count)
{
    char line[ADC_LINE_MAX];
    tth_u64 total, avail;
    FILE *fp;

    fp = fopen(local, "rb");
    if (!fp) {
        send_line(p, "CSTA 151 File\\snot\\savailable");
        return 0;
    }

    /* The size has to come from the file itself; nothing else knows it. */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        send_line(p, "CSTA 151 File\\snot\\savailable");
        return 0;
    }
    total = (tth_u64)ftell(fp);

    if (offset > total) {
        fclose(fp);
        send_line(p, "CSTA 152 Invalid\\soffset");
        return 0;
    }
    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        send_line(p, "CSTA 152 Invalid\\soffset");
        return 0;
    }

    avail = total - offset;
    if (count >= 0 && (tth_u64)count < avail)
        avail = (tth_u64)count;

    p->fp = fp;
    snprintf(p->sending_type, sizeof p->sending_type, "%s", wire_type);
    snprintf(p->name, sizeof p->name, "%s", display);
    p->tth[0] = '\0';
    p->size = avail;
    p->moved = 0;
    p->offset = offset;
    p->uploading = 1;
    p->out_len = 0;
    p->out_off = 0;

    snprintf(line, sizeof line, "CSND %s %s %lu %lu",
             wire_type, wire_ident,
             (unsigned long)offset, (unsigned long)avail);
    if (send_line(p, line) != 0)
        return -1;

    p->state = PEER_SENDING;
    return 1;
}

static int begin_send(peerconn *p, const char *tth, tth_u64 offset,
                      long long count)
{
    share_file *f;
    char line[ADC_LINE_MAX];
    tth_u64 avail;

    if (!p->share) {
        send_line(p, "CSTA 151 File\\snot\\savailable");
        return 0;
    }

    f = share_find_tth_b32(p->share, tth);
    if (!f) {
        /* 51 is "file not available". Answering properly matters: a peer
           that gets silence retries forever. */
        send_line(p, "CSTA 151 File\\snot\\savailable");
        return 0;
    }

    if (offset > f->size) {
        send_line(p, "CSTA 152 Invalid\\soffset");
        return 0;
    }

    avail = f->size - offset;
    if (count >= 0 && (tth_u64)count < avail)
        avail = (tth_u64)count;

    p->fp = fopen(share_local(p->share, f), "rb");
    if (!p->fp) {
        send_line(p, "CSTA 151 File\\snot\\savailable");
        return 0;
    }
    if (offset && fseek((FILE *)p->fp, (long)offset, SEEK_SET) != 0) {
        fclose((FILE *)p->fp);
        p->fp = NULL;
        send_line(p, "CSTA 152 Invalid\\soffset");
        return 0;
    }

    snprintf(p->name, sizeof p->name, "%s", share_name(p->share, f));
    snprintf(p->tth, sizeof p->tth, "%s", tth);
    p->size = avail;
    p->moved = 0;
    p->offset = offset;
    p->uploading = 1;
    p->out_len = 0;
    p->out_off = 0;

    /* A connection serves several things in a row -- leaf hashes, then the
       file -- so this has to be set here too, not only where the lists and
       hashes are served. It was not, and a file upload reported itself as
       "tthl" using the value left over from the request before it. */
    snprintf(p->sending_type, sizeof p->sending_type, "%s", "file");

    snprintf(line, sizeof line, "CSND file TTH/%s %lu %lu",
             tth, (unsigned long)offset, (unsigned long)avail);
    if (send_line(p, line) != 0)
        return -1;

    p->state = PEER_SENDING;
    return 1;
}

static int send_bytes(peerconn *p, unsigned long now)
{
    int guard;
    int moved = 0;

    for (guard = 0; guard < 8; guard++) {
        int n;

        /* Refill only once the socket has taken everything we staged:
           otherwise a partial write would lose the remainder. */
        if (p->out_off >= p->out_len) {
            tth_u64 left = p->size - p->moved;
            size_t want = sizeof p->out;
            size_t got;

            if (left == 0)
                break;
            if ((tth_u64)want > left)
                want = (size_t)left;

            got = fread(p->out, 1, want, (FILE *)p->fp);
            if (got == 0) {
                fail(p, "short read while serving");
                return -1;
            }
            p->out_len = got;
            p->out_off = 0;
        }

        /* A short write is normal, not an error. The old code used
           tp_write_all here, which spins until the socket drains -- that
           stalls the whole client for the length of an upload, and against
           a peer that stops reading it eventually failed the transfer
           outright. */
        n = tp_write(&p->t, p->out + p->out_off, p->out_len - p->out_off);
        if (n == TP_AGAIN)
            break;                  /* socket full; resume next poll */
        if (n < 0) {
            fail(p, "the peer stopped reading");
            return -1;
        }

        p->out_off += (size_t)n;
        p->moved += (tth_u64)n;
        p->last_activity = now;
        moved = 1;
    }

    if (p->moved >= p->size) {
        fclose((FILE *)p->fp);
        p->fp = NULL;

        /* Back to idle, not done: a peer fetching a file in parts asks
           again on the same connection, and real clients do. Closing after
           each part made a segmented download reconnect for every segment
           -- visible as one CSUP/CINF handshake per kilobyte. The
           connection now closes when the peer closes it. */
        p->state = PEER_IDLE;
        p->want = PEER_WANT_NOTHING;
        p->out_len = 0;
        p->out_off = 0;

        if (p->cb.on_complete)
            p->cb.on_complete(p->cb.user, p, p->local);
        return 1;
    }

    if (moved && p->cb.on_progress)
        p->cb.on_progress(p->cb.user, p);

    return moved;
}

/* ---------------------------------------------------------- dispatching */

/* Returns 0 when handled, 1 when a raw-byte transfer has just begun, and
   -1 on an error that has already failed the connection. */
static int dispatch(peerconn *p, const adc_msg *m)
{
    if (m->type == 0)
        return 0;                   /* keepalive */

    if (strcmp(m->cmd, "SUP") == 0) {
        if (p->state != PEER_SUP)
            return 0;
        p->state = PEER_INF;
        return send_our_inf(p);
    }

    if (strcmp(m->cmd, "INF") == 0) {
        const char *cid = adc_field(m, "ID");
        const char *tok = adc_field(m, "TO");

        if (cid)
            snprintf(p->peer_cid, sizeof p->peer_cid, "%s", cid);
        if (tok && !p->token[0])
            snprintf(p->token, sizeof p->token, "%s", tok);

        /* Identified. If we dialled out to fetch something, ask now; if
           they dialled us, wait to be asked -- unless they are answering a
           CTM of ours, which the token tells us. */
        if (p->state == PEER_INF || p->state == PEER_SUP) {
            p->state = PEER_IDLE;

            if (!p->incoming && p->want != PEER_WANT_NOTHING)
                return request_file(p);

            /* They dialled us carrying a token. If it is one of ours, this
               connection is the download we asked for and the caller fills
               in what to fetch. */
            if (p->incoming && tok && tok[0] && p->cb.on_token &&
                p->cb.on_token(p->cb.user, p, tok)) {
                p->uploading = 0;
                return request_file(p);
            }
        }
        return 0;
    }

    if (strcmp(m->cmd, "GET") == 0) {
        /* CGET <type> <identifier> <start> <bytes> */
        const char *ident;
        tth_u64 start;
        long long count;

        if (m->nparam < 4) {
            send_line(p, "CSTA 140 Malformed\\sGET");
            return 0;
        }
        ident = m->param[1];
        start = (tth_u64)strtoul(m->param[2], NULL, 10);
        count = strtol(m->param[3], NULL, 10);

        /* A file list. "list /" means the whole share, which is already
           written out and compressed, so it is served from there. Anything
           else is a partial list: one directory, generated on the spot,
           because keeping a per-directory list for a share nobody may
           browse would be work and memory spent on a guess.
           
           RE1 asks for the subtree rather than the one level. A client
           that sends it for the root is asking for the whole list the long
           way round, and gets exactly that. */
        if (strcmp(m->param[0], "list") == 0) {
            int recursive = 0;
            int k;

            for (k = 4; k < m->nparam; k++)
                if (strcmp(m->param[k], "RE1") == 0)
                    recursive = 1;

            /* --refuse-partial: every `CGET list` is refused, including the
               root, because that is what a client predating partial lists
               does. Refusing only the subdirectories would exercise the
               fallback from a place no real client falls back from. */
            if (p->no_serve_partial) {
                send_line(p,
                          "CSTA 151 Partial\\sfile\\slists\\snot\\ssupported");
                return 0;
            }

            if (ident[0] == '\0' || strcmp(ident, "/") == 0) {
                if (!recursive && p->share) {
                    /* A non-recursive list of the root is the one case
                       where the published list is the wrong answer: it
                       holds everything, and the peer asked for one
                       level. */
                    int n = filelist_publish_partial(
                                p->share, "/", 0, FILELIST_PART_XML,
                                FILELIST_PART_BZ2, p->id.cid_b32);

                    if (n < 0) {
                        send_line(p,
                                  "CSTA 151 File\\slist\\snot\\savailable");
                        return 0;
                    }
                    return begin_send_path(p, FILELIST_PART_BZ2, "list",
                                           "/", "our file list",
                                           start, count);
                }
                return begin_send_path(p, FILELIST_BZ2, "file",
                                       FILELIST_WIRE_NAME, "our file list",
                                       start, count);
            }

            if (!p->share) {
                send_line(p, "CSTA 151 Nothing\\sshared");
                return 0;
            }

            /* The path is matched against virtual paths and never used to
               open anything, so a hostile one is a miss rather than a
               traversal -- but it is still clamped, because an absurdly
               long one would be copied into a fixed buffer. */
            if (strlen(ident) >= PEER_PATH_MAX) {
                send_line(p, "CSTA 151 Directory\\snot\\sshared");
                return 0;
            }

            {
                int n = filelist_publish_partial(
                            p->share, ident, recursive, FILELIST_PART_XML,
                            FILELIST_PART_BZ2, p->id.cid_b32);

                if (n < 0) {
                    /* Not shared, which is a different answer from shared
                       and empty: a client asking for a directory we do not
                       have should be told so rather than shown nothing. */
                    send_line(p, "CSTA 151 Directory\\snot\\sshared");
                    return 0;
                }

                return begin_send_path(p, FILELIST_PART_BZ2, "list",
                                       ident, "a directory listing",
                                       start, count);
            }
        }

        /* Leaf hashes. A real client asks for these before a transfer so
           it can verify parts of the file independently, and refusing it
           is not free: AirDC++ reads "file not available" as us not having
           the file and drops us as a source entirely. */
        if (strcmp(m->param[0], "tthl") == 0) {
            share_file *f;
            long n;
            int level = 0;

            if (!p->share || strncmp(ident, "TTH/", 4) != 0) {
                send_line(p, "CSTA 151 File\\snot\\savailable");
                return 0;
            }

            f = share_find_tth_b32(p->share, ident + 4);
            if (!f) {
                send_line(p, "CSTA 151 File\\snot\\savailable");
                return 0;
            }

            n = share_write_tthl(p->share, f, TTHL_FILE, &level);
            if (n <= 0) {
                send_line(p, "CSTA 151 Leaf\\shashes\\snot\\savailable");
                return 0;
            }

            return begin_send_path(p, TTHL_FILE, "tthl", ident,
                                   "leaf hashes", start, count);
        }

        if (strcmp(m->param[0], "file") != 0) {
            send_line(p, "CSTA 151 Unsupported\\srequest\\stype");
            return 0;
        }

        if (strcmp(ident, FILELIST_WIRE_NAME) == 0)
            return begin_send_path(p, FILELIST_BZ2, "file",
                                   FILELIST_WIRE_NAME, "our file list",
                                   start, count);

        if (strncmp(ident, "TTH/", 4) != 0) {
            /* We only serve by hash. Serving by path would mean trusting a
               path from the network to address our filesystem. */
            send_line(p, "CSTA 151 Only\\sTTH\\srequests\\ssupported");
            return 0;
        }

        /* Slots are the caller's policy. Only files take one: a file list
           is how someone decides what to ask for, and charging a slot for
           it makes a client look offline to everyone browsing.
           Severity 1 (recoverable) and code 53 per the ADC status table:
           the peer should try again later, not give up on us. */
        if (p->cb.may_upload && !p->cb.may_upload(p->cb.user, p)) {
            send_line(p, "CSTA 153 All\\supload\\sslots\\sare\\sbusy");
            return 0;
        }

        return begin_send(p, ident + 4, start, count);
    }

    if (strcmp(m->cmd, "SND") == 0) {
        /* CSND <type> <identifier> <start> <bytes>, then raw data. */
        tth_u64 declared;

        if (p->state != PEER_GETTING) {
            fail(p, "unexpected CSND");
            return -1;
        }
        if (m->nparam < 4) {
            fail(p, "malformed CSND");
            return -1;
        }

        declared = (tth_u64)strtoul(m->param[3], NULL, 10);
        if (declared == 0) {
            fail(p, "the peer offered zero bytes");
            return -1;
        }

        return begin_receive(p, declared);
    }

    if (strcmp(m->cmd, "STA") == 0) {
        char why[PEER_ERR_MAX];

        /* Severity is the first digit: 0 is informational and says nothing
           is wrong. AirDC++ sends "CSTA 000  RFadc://<hub>" to say which
           hub a connection came from, and treating that as a refusal left
           a spurious error on a connection that was working fine. */
        if (m->nparam >= 1 && m->param[0][0] == '0')
            return 0;

        snprintf(why, sizeof why, "peer refused: %s",
                 m->nparam >= 2 ? m->param[1] : "no reason given");

        /* A refusal of a partial list is worth telling apart from every
           other refusal: it means "ask me for the whole thing instead",
           not "go away". Older clients and some hubsoftware answer any
           `CGET list <dir>` with a CSTA. */
        if (p->want == PEER_WANT_LIST && p->list_base[0])
            p->list_refused = 1;

        /* Severity 2 is fatal, 1 recoverable -- but for a transfer either
           means this attempt is over. */
        if (p->state == PEER_GETTING || p->state == PEER_DOWNLOADING) {
            fail(p, why);
            return -1;
        }
        snprintf(p->err, sizeof p->err, "%s", why);
        return 0;
    }

    return 0;                       /* unknown: ignore, do not disconnect */
}

int peer_list_refused(const peerconn *p)
{
    return p->list_refused;
}

int peer_poll(peerconn *p, unsigned long now)
{
    char buf[1024];
    char line[ADC_LINE_MAX];
    int progress = 0;
    int guard;

    if (p->state == PEER_DONE || p->state == PEER_FAILED)
        return -1;

    rate_tick(p, now);

    /* Raw byte phases bypass the line framer entirely: the data is not
       newline-delimited and must not be scanned for one. */
    if (p->state == PEER_DOWNLOADING)
        return receive_bytes(p, now);
    if (p->state == PEER_SENDING)
        return send_bytes(p, now);

    for (guard = 0; guard < 16; guard++) {
        int n = tp_read(&p->t, buf, sizeof buf);

        if (n == TP_AGAIN)
            break;
        if (n == TP_CLOSED) {
            if (p->state == PEER_IDLE || p->state == PEER_DONE)
                p->state = PEER_DONE;      /* an orderly goodbye */
            else
                fail(p, "the peer closed the connection");
            return -1;
        }
        if (n < 0) {
            fail(p, "read error");
            return -1;
        }

        progress = 1;
        p->last_activity = now;

        if (linebuf_feed(&p->in, buf, (size_t)n) != 0)
            continue;                      /* over-long line; resynced */

        while (linebuf_next(&p->in, line)) {
            adc_msg m;

            tr(p, "<--", line);

            if (adc_parse(&m, line) != 0)
                continue;                  /* junk: ignore */

            /* Only a negative return is an error. begin_send() and
               begin_receive() answer 1 for "a transfer just started", and
               treating that as a failure dropped the connection the instant
               CSND went out -- the upload closed before a byte moved. */
            if (dispatch(p, &m) < 0)
                return -1;

            /* A transfer may have started mid-batch, and the raw bytes
               could already be in our socket buffer. */
            if (p->state == PEER_DOWNLOADING)
                return receive_bytes(p, now);
            if (p->state == PEER_SENDING)
                return send_bytes(p, now);
            if (p->state == PEER_DONE || p->state == PEER_FAILED)
                return -1;
        }
    }

    return progress;
}
