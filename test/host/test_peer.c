/* test_peer.c - the client-to-client connection, both ends at once.
 *
 * Two peerconns are wired back to back over an in-memory transport pair and
 * polled in turn until the file has moved. That is the only way to cover
 * this code without two processes, a hub and a listening socket -- and the
 * absence of such a test let a one-character bug ship: peer_poll treated
 * dispatch()'s "a transfer just started" return (1) as an error, so an
 * upload closed the connection immediately after sending CSND. The
 * downloader saw "the peer closed mid-transfer" and no byte ever moved.
 *
 * The file is real, under the system temp directory, because the upload
 * path is fopen/fread and the download path verifies the TTH as it writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/adc/peerconn.h"
#include "../../src/share/filelist.h"
#include "../../src/share/bz.h"
#include "../../src/hash/tth.h"

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                                              \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------- a transport pair
 *
 * Two byte queues, one per direction. Deliberately small so a transfer of
 * any size has to survive a full write buffer and TP_AGAIN, which is where
 * the real bugs live. */
#define PIPE_CAP 4096

typedef struct {
    unsigned char buf[PIPE_CAP];
    size_t len;
    int    closed;
} bytepipe;

typedef struct {
    bytepipe *rx;       /* we read from here */
    bytepipe *tx;       /* we write to here */
} pipe_end;

static int pipe_read(transport *t, void *buf, size_t len)
{
    pipe_end *e = (pipe_end *)t->impl;
    bytepipe *q = e->rx;
    size_t n;

    if (q->len == 0)
        return q->closed ? TP_CLOSED : TP_AGAIN;

    n = (len < q->len) ? len : q->len;
    memcpy(buf, q->buf, n);
    memmove(q->buf, q->buf + n, q->len - n);
    q->len -= n;
    return (int)n;
}

static int pipe_write(transport *t, const void *buf, size_t len)
{
    pipe_end *e = (pipe_end *)t->impl;
    bytepipe *q = e->tx;
    size_t room = PIPE_CAP - q->len;
    size_t n;

    if (q->closed)
        return TP_ERROR;
    if (room == 0)
        return TP_AGAIN;

    n = (len < room) ? len : room;
    memcpy(q->buf + q->len, buf, n);
    q->len += n;
    return (int)n;
}

static int pipe_pending(transport *t)
{
    pipe_end *e = (pipe_end *)t->impl;
    return (int)e->rx->len;
}

static void pipe_close(transport *t)
{
    pipe_end *e = (pipe_end *)t->impl;
    e->tx->closed = 1;
    t->fd = -1;
}

static void pipe_pair(transport *a, transport *b,
                      pipe_end *ea, pipe_end *eb,
                      bytepipe *ab, bytepipe *ba)
{
    memset(ab, 0, sizeof *ab);
    memset(ba, 0, sizeof *ba);

    ea->tx = ab; ea->rx = ba;
    eb->tx = ba; eb->rx = ab;

    a->t_read = pipe_read; a->t_write = pipe_write;
    a->t_pending = pipe_pending; a->t_close = pipe_close;
    a->fd = 100; a->impl = ea;

    b->t_read = pipe_read; b->t_write = pipe_write;
    b->t_pending = pipe_pending; b->t_close = pipe_close;
    b->fd = 101; b->impl = eb;
}

/* ------------------------------------------------------------- the fixture */

static char g_dir[256];
static char g_src[320];
static char g_dst[320];

static void make_source(tth_u64 size, char *tth_out)
{
    FILE *f = fopen(g_src, "wb");
    tth_ctx ctx;
    unsigned char root[24];
    unsigned long i;

    if (!f) {
        printf("FAIL: cannot write %s\n", g_src);
        g_fail++;
        return;
    }

    /* Not all one byte value: a wrong offset or a dropped chunk has to
       change the hash, and a file of zeros hides exactly that. */
    tth_init(&ctx, NULL, NULL, 0);
    for (i = 0; i < (unsigned long)size; i++) {
        unsigned char c = (unsigned char)((i * 31u + (i >> 8)) & 0xFF);
        fputc(c, f);
        tth_update(&ctx, &c, 1);
    }
    fclose(f);

    tth_final(&ctx, root);
    tth_to_base32(root, tth_out);
}

static int g_sent;
static int g_completed;
static int g_failed;
static char g_failreason[PEER_ERR_MAX];

static void on_complete(void *user, peerconn *p, const char *local)
{
    (void)user; (void)p; (void)local;
    g_completed++;
}

/* The uploading end reports completion too, so it gets its own callbacks:
   sharing one set makes "the download finished" and "the upload finished"
   indistinguishable. */
static void on_sent(void *user, peerconn *p, const char *local)
{
    (void)user; (void)p; (void)local;
    g_sent++;
}

static void on_failed(void *user, peerconn *p)
{
    (void)user;
    g_failed++;
    snprintf(g_failreason, sizeof g_failreason, "%s", peer_error(p));
}

/* Poll both ends until neither makes progress or the limit is hit. The
   limit is generous but finite: a state machine that livelocks must fail
   the test rather than hang it. */
static int pump(peerconn *dn, peerconn *up, int max_rounds)
{
    int i;

    for (i = 0; i < max_rounds; i++) {
        int a = peer_poll(dn, (unsigned long)i);
        int b = peer_poll(up, (unsigned long)i);

        if (dn->state == PEER_DONE || dn->state == PEER_FAILED)
            return i;
        if (a <= 0 && b <= 0 && dn->state == PEER_IDLE)
            return i;           /* nothing left to do */
    }
    return -1;
}

/* ------------------------------------------------------------------ cases */

static void test_download(tth_u64 size)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb;
    share_index ix;
    share_file *f;
    adc_identity id_dn, id_up;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    char got_tth[BASE32_TIGER_LEN + 1];
    int rounds;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';

    make_source(size, tth);
    remove(g_dst);

    /* The uploader's share: one file, already hashed. An unhashed file is
       deliberately unfindable, so this must be set. */
    CHECK(share_init(&ix) == 0, "share_init failed");
    f = share_add_file(&ix, "up/MAZERUN2.ZIP", g_src, size, 0);
    CHECK(f != NULL, "share_add_file failed");
    if (!f) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, f, raw);

    adc_identity_generate(&id_dn, 1);
    adc_identity_generate(&id_up, 2);

    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;

    {
        peer_callbacks upcb;

        memset(&upcb, 0, sizeof upcb);
        upcb.on_complete = on_sent;
        upcb.on_failed = on_failed;
        CHECK(peer_accept(&up, &tb, &id_up, &ix, &upcb, 0) == 0,
              "peer_accept failed");
    }
    CHECK(peer_start_download(&dn, &ta, &id_dn, "T1", tth, "MAZERUN2.ZIP",
                              size, g_dst, &cb, 0) == 0,
          "peer_start_download failed");

    rounds = pump(&dn, &up, 20000);

    CHECK(rounds >= 0, "the transfer never settled (livelock?)");
    CHECK(g_failed == 0, "transfer failed: %s", g_failreason);
    CHECK(dn.state == PEER_DONE, "downloader ended in %s (%s)",
          peer_state_name(dn.state), peer_error(&dn));
    CHECK(g_completed == 1, "on_complete fired %d times", g_completed);
    CHECK(g_sent == 1, "the uploader reported completion %d times", g_sent);
    CHECK(up.moved == size, "the uploader sent %lu of %lu bytes",
          (unsigned long)up.moved, (unsigned long)size);
    CHECK(dn.moved == size, "moved %lu of %lu bytes",
          (unsigned long)dn.moved, (unsigned long)size);

    /* The bytes on disk, hashed independently of the client's own
       verification: "it said it matched" is not the same claim. */
    {
        FILE *g = fopen(g_dst, "rb");
        tth_ctx ctx;
        unsigned char root[24];
        unsigned char buf[1024];
        size_t n;

        CHECK(g != NULL, "the download was not written to %s", g_dst);
        if (g) {
            tth_init(&ctx, NULL, NULL, 0);
            while ((n = fread(buf, 1, sizeof buf, g)) > 0)
                tth_update(&ctx, buf, n);
            fclose(g);
            tth_final(&ctx, root);
            tth_to_base32(root, got_tth);
            CHECK(strcmp(got_tth, tth) == 0,
                  "content hash %s, expected %s", got_tth, tth);
        }
    }

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
}

/* A peer asking for a hash we do not have must be refused with CSTA, not
   dropped: the downloader has other sources to try and needs to be told. */
static void test_unknown_hash(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb;
    share_index ix;
    adc_identity id_dn, id_up;
    const char *absent = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    g_completed = g_failed = 0;
    g_failreason[0] = '\0';

    CHECK(share_init(&ix) == 0, "share_init failed");
    adc_identity_generate(&id_dn, 1);
    adc_identity_generate(&id_up, 2);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;

    peer_accept(&up, &tb, &id_up, &ix, &cb, 0);
    peer_start_download(&dn, &ta, &id_dn, "T2", absent, "GONE.ZIP",
                        1024, g_dst, &cb, 0);

    pump(&dn, &up, 2000);

    CHECK(dn.state == PEER_FAILED, "an absent file left us in %s",
          peer_state_name(dn.state));
    CHECK(strstr(peer_error(&dn), "refused") != NULL,
          "expected a refusal, got: %s", peer_error(&dn));
    CHECK(g_completed == 0, "on_complete fired for a file we never got");

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
}

/* Serving a file list. The list is a file on disk with a name nobody can
   look up by hash, so it goes through a different path from every other
   transfer -- which is exactly why it needs its own test. */
static void test_file_list(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb, upcb;
    share_index ix;
    adc_identity id_dn, id_up;
    char got[384];
    FILE *f;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';

    /* The file list as it is stored locally: the name on the wire is the
       long one, the file on disk is 8.3. */
    f = fopen(FILELIST_BZ2, "wb");
    CHECK(f != NULL, "cannot create a fixture file list");
    if (!f)
        return;
    fputs("BZh1-not-really-compressed-but-bytes-are-bytes", f);
    fclose(f);

    snprintf(got, sizeof got, "%s/dosypp_peer_list.bz2", g_dir);
    remove(got);

    CHECK(share_init(&ix) == 0, "share_init failed");
    adc_identity_generate(&id_dn, 3);
    adc_identity_generate(&id_up, 4);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;

    /* An empty share on the serving side: a file list must be served even
       by someone sharing nothing, because that is how you find out. */
    CHECK(peer_accept(&up, &tb, &id_up, &ix, &upcb, 0) == 0,
          "peer_accept failed");
    CHECK(peer_start_list(&dn, &ta, &id_dn, "L1", got, NULL, &cb, 0) == 0,
          "peer_start_list failed");

    pump(&dn, &up, 20000);

    CHECK(dn.state == PEER_DONE, "fetching a list ended in %s (%s)",
          peer_state_name(dn.state), peer_error(&dn));
    CHECK(g_completed == 1, "the list did not complete (%d)", g_completed);
    CHECK(dn.want == PEER_WANT_LIST,
          "the connection forgot it wanted a list");

    /* It has to arrive byte for byte, and it must NOT be hash-checked --
       a file list has no hash, and verifying it against an empty TTH
       would delete it as a mismatch. */
    f = fopen(got, "rb");
    CHECK(f != NULL, "the list was not written to disk");
    if (f) {
        char buf[128];
        size_t n = fread(buf, 1, sizeof buf - 1, f);

        buf[n] = '\0';
        fclose(f);
        CHECK(strcmp(buf, "BZh1-not-really-compressed-but-bytes-are-bytes")
              == 0, "the list arrived as \"%s\"", buf);
    }

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
    remove(got);
    remove(FILELIST_BZ2);
}

/* Upload slots. Refusing has to be a status the peer can read, not a
   dropped connection, and a file list must never be refused for want of a
   slot -- otherwise a busy client looks like an empty one. */
static int g_allow_upload;

static int may_upload_cb(void *user, peerconn *p)
{
    (void)user; (void)p;
    return g_allow_upload;
}

static void test_slots(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb, upcb;
    share_index ix;
    share_file *sf;
    adc_identity id_dn, id_up;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    FILE *f;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';
    g_allow_upload = 0;

    make_source(4096, tth);

    CHECK(share_init(&ix) == 0, "share_init failed");
    sf = share_add_file(&ix, "up/A.ZIP", g_src, 4096, 0);
    CHECK(sf != NULL, "share_add_file failed");
    if (!sf) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, sf, raw);

    adc_identity_generate(&id_dn, 5);
    adc_identity_generate(&id_up, 6);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    peer_accept(&up, &tb, &id_up, &ix, &upcb, 0);
    peer_start_download(&dn, &ta, &id_dn, "T5", tth, "A.ZIP", 4096,
                        g_dst, &cb, 0);

    pump(&dn, &up, 4000);

    CHECK(dn.state == PEER_FAILED, "a refused request left us in %s",
          peer_state_name(dn.state));
    CHECK(strstr(peer_error(&dn), "slots") != NULL ||
          strstr(peer_error(&dn), "busy") != NULL,
          "the refusal did not mention slots: %s", peer_error(&dn));
    CHECK(g_completed == 0, "a refused download reported success");

    /* The serving side must still be usable: refusing a request is not a
       reason to drop the connection, because the peer may well ask for
       something else -- or the same thing once a slot frees up. */
    CHECK(up.state != PEER_FAILED, "refusing a request killed the upload "
          "connection (%s)", peer_state_name(up.state));

    peer_close(&dn);
    peer_close(&up);

    /* And a file list is served anyway, with no slots free at all. */
    g_completed = g_failed = 0;
    f = fopen(FILELIST_BZ2, "wb");
    if (f) { fputs("BZh1list", f); fclose(f); }

    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    peer_accept(&up, &tb, &id_up, &ix, &upcb, 0);
    peer_start_list(&dn, &ta, &id_dn, "L2", g_dst, NULL, &cb, 0);

    pump(&dn, &up, 4000);

    CHECK(dn.state == PEER_DONE,
          "a file list was refused for want of a slot: %s",
          peer_error(&dn));
    CHECK(g_completed == 1, "the list did not arrive (%d)", g_completed);

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
    remove(FILELIST_BZ2);
}

/* Resuming. The bytes already on disk are re-hashed so the finished file
   is still verified -- the whole point of addressing by hash. */
static void test_resume(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb, upcb;
    share_index ix;
    share_file *sf;
    adc_identity id_dn, id_up;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    const tth_u64 size = 40000;
    const long partial = 15000;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';
    g_allow_upload = 1;

    make_source(size, tth);

    /* A partial download, as an interrupted run would have left it. */
    {
        FILE *in = fopen(g_src, "rb");
        FILE *out = fopen(g_dst, "wb");
        char buf[4096];
        long left = partial;

        CHECK(in && out, "cannot build the partial fixture");
        if (!in || !out) {
            if (in) fclose(in);
            if (out) fclose(out);
            return;
        }
        while (left > 0) {
            size_t want = (size_t)(left < (long)sizeof buf
                                   ? left : (long)sizeof buf);
            size_t n = fread(buf, 1, want, in);
            if (n == 0) break;
            fwrite(buf, 1, n, out);
            left -= (long)n;
        }
        fclose(in);
        fclose(out);
    }

    CHECK(share_init(&ix) == 0, "share_init failed");
    sf = share_add_file(&ix, "up/BIG.ZIP", g_src, size, 0);
    if (!sf) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, sf, raw);

    adc_identity_generate(&id_dn, 7);
    adc_identity_generate(&id_up, 8);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    peer_accept(&up, &tb, &id_up, &ix, &upcb, 0);
    peer_start_download(&dn, &ta, &id_dn, "T6", tth, "BIG.ZIP", size,
                        g_dst, &cb, 0);
    dn.offset = (tth_u64)partial;        /* what the queue would set */

    pump(&dn, &up, 40000);

    CHECK(g_failed == 0, "the resumed transfer failed: %s", g_failreason);
    CHECK(dn.state == PEER_DONE, "resuming ended in %s (%s)",
          peer_state_name(dn.state), peer_error(&dn));

    /* Only the missing part should have crossed the wire. */
    CHECK(up.moved == size - (tth_u64)partial,
          "the peer sent %lu bytes, expected %lu",
          (unsigned long)up.moved, (unsigned long)(size - partial));

    /* And the result is the whole file, verified. */
    {
        FILE *g = fopen(g_dst, "rb");
        tth_ctx ctx;
        unsigned char root[24];
        unsigned char buf[1024];
        char got_tth[BASE32_TIGER_LEN + 1];
        size_t n;
        long sz = 0;

        CHECK(g != NULL, "the resumed file is missing");
        if (g) {
            tth_init(&ctx, NULL, NULL, 0);
            while ((n = fread(buf, 1, sizeof buf, g)) > 0) {
                tth_update(&ctx, buf, n);
                sz += (long)n;
            }
            fclose(g);
            tth_final(&ctx, root);
            tth_to_base32(root, got_tth);

            CHECK(sz == (long)size, "the resumed file is %ld bytes, "
                  "expected %lu", sz, (unsigned long)size);
            CHECK(strcmp(got_tth, tth) == 0,
                  "the resumed file hashes to %s, expected %s",
                  got_tth, tth);
        }
    }

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
}

/* A resumed download whose partial file was corrupted must fail the hash,
   not be accepted. Without re-hashing the prefix this test passes while
   the client is quietly wrong. */
static void test_resume_corrupt(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb, upcb;
    share_index ix;
    share_file *sf;
    adc_identity id_dn, id_up;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    const tth_u64 size = 20000;
    const long partial = 8000;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';
    g_allow_upload = 1;

    make_source(size, tth);

    {
        FILE *out = fopen(g_dst, "wb");
        long i;

        CHECK(out != NULL, "cannot build the corrupt fixture");
        if (!out)
            return;
        /* The right length, the wrong bytes. */
        for (i = 0; i < partial; i++)
            fputc(0xAA, out);
        fclose(out);
    }

    CHECK(share_init(&ix) == 0, "share_init failed");
    sf = share_add_file(&ix, "up/BIG.ZIP", g_src, size, 0);
    if (!sf) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, sf, raw);

    adc_identity_generate(&id_dn, 9);
    adc_identity_generate(&id_up, 10);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    peer_accept(&up, &tb, &id_up, &ix, &upcb, 0);
    peer_start_download(&dn, &ta, &id_dn, "T7", tth, "BIG.ZIP", size,
                        g_dst, &cb, 0);
    dn.offset = (tth_u64)partial;

    pump(&dn, &up, 40000);

    CHECK(dn.state == PEER_FAILED,
          "a corrupt resume was accepted (state %s)",
          peer_state_name(dn.state));
    CHECK(strstr(peer_error(&dn), "hash") != NULL,
          "the failure was not about the hash: %s", peer_error(&dn));
    CHECK(g_completed == 0, "a corrupt resume reported success");

    /* And the bad file is gone rather than left to be shared on. */
    {
        FILE *g = fopen(g_dst, "rb");

        CHECK(g == NULL, "the corrupt file was left on disk");
        if (g)
            fclose(g);
    }

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
}

/* The case a real client found: we are ACTIVE, so we send CTM and the peer
   dials US. The only thing tying that inbound connection to what we asked
   for is the token it quotes in its CINF. Every test above has the
   downloader dialling out, which is why none of them noticed that an active
   client could not download at all.
 *
   Here the "inbound" side is the one being tested: `dn` is accepted rather
   than started as a download, and only the token makes it one. */
static char g_claim_tth[BASE32_TIGER_LEN + 1];
static tth_u64 g_claim_size;
static int g_claim_calls;

static int claim_token(void *user, peerconn *p, const char *token)
{
    (void)user;

    g_claim_calls++;

    /* What the queue does: recognise the token and say what the connection
       is for. */
    if (strcmp(token, "T42") != 0)
        return 0;

    p->want = PEER_WANT_FILE;
    snprintf(p->tth, sizeof p->tth, "%s", g_claim_tth);
    snprintf(p->name, sizeof p->name, "%s", "CLAIMED.ZIP");
    snprintf(p->local, sizeof p->local, "%s", g_dst);
    p->size = g_claim_size;
    p->offset = 0;
    return 1;
}

static void test_inbound_token(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn dn, up;
    peer_callbacks cb, upcb;
    share_index ix;
    share_file *sf;
    adc_identity id_dn, id_up;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    const tth_u64 size = 30000;

    g_sent = g_completed = g_failed = 0;
    g_failreason[0] = '\0';
    g_claim_calls = 0;
    g_allow_upload = 1;

    make_source(size, tth);
    remove(g_dst);
    snprintf(g_claim_tth, sizeof g_claim_tth, "%s", tth);
    g_claim_size = size;

    CHECK(share_init(&ix) == 0, "share_init failed");
    sf = share_add_file(&ix, "up/CLAIMED.ZIP", g_src, size, 0);
    if (!sf) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, sf, raw);

    adc_identity_generate(&id_dn, 11);
    adc_identity_generate(&id_up, 12);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    cb.on_token = claim_token;          /* the side that was dialled */

    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    /* We accepted a connection, as an active client does... */
    CHECK(peer_accept(&dn, &ta, &id_dn, &ix, &cb, 0) == 0,
          "peer_accept failed");

    /* ...and the peer that dialled us is fetching nothing itself: it is
       answering our CTM, so it quotes our token and waits to be asked.
       That is exactly peer_start_serving, which is what the client uses
       for this role. */
    CHECK(peer_start_serving(&up, &tb, &id_up, "T42", &ix, &upcb, 0) == 0,
          "the dialling side failed");

    pump(&dn, &up, 40000);

    CHECK(g_claim_calls >= 1, "the token was never offered to the owner");
    CHECK(dn.uploading == 0,
          "the claimed connection is still marked as an upload");
    CHECK(g_failed == 0, "the claimed download failed: %s", g_failreason);
    CHECK(dn.state == PEER_DONE, "the claimed download ended in %s (%s)",
          peer_state_name(dn.state), peer_error(&dn));
    CHECK(dn.moved == size, "moved %lu of %lu bytes",
          (unsigned long)dn.moved, (unsigned long)size);

    {
        FILE *g = fopen(g_dst, "rb");
        tth_ctx ctx;
        unsigned char root[24];
        unsigned char buf[1024];
        char got_tth[BASE32_TIGER_LEN + 1];
        size_t n;

        CHECK(g != NULL, "nothing was written");
        if (g) {
            tth_init(&ctx, NULL, NULL, 0);
            while ((n = fread(buf, 1, sizeof buf, g)) > 0)
                tth_update(&ctx, buf, n);
            fclose(g);
            tth_final(&ctx, root);
            tth_to_base32(root, got_tth);
            CHECK(strcmp(got_tth, tth) == 0,
                  "the claimed download hashes to %s, expected %s",
                  got_tth, tth);
        }
    }

    /* A token we do not recognise must leave the connection alone: it is
       somebody asking us for something, not an answer to anything. */
    g_claim_calls = 0;
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    peer_accept(&dn, &ta, &id_dn, &ix, &cb, 0);
    peer_start_serving(&up, &tb, &id_up, "NOTOURS", &ix, &upcb, 0);
    pump(&dn, &up, 2000);
    CHECK(g_claim_calls >= 1, "the unknown token was not offered either");
    CHECK(dn.state != PEER_FAILED,
          "an unrecognised token broke the connection: %s",
          peer_error(&dn));

    peer_close(&dn);
    peer_close(&up);
    share_free(&ix);
}

/* A peer asking for leaf hashes over a real connection. AirDC++ asks for
   these before it transfers, and a refusal made it drop us as a source --
   so the exchange itself is worth a test, not just the computation. */
static void test_tthl_request(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn asker, server;
    peer_callbacks cb, upcb;
    share_index ix;
    share_file *sf;
    adc_identity id_a, id_b;
    char tth[BASE32_TIGER_LEN + 1];
    unsigned char raw[24];
    char line[160];
    const tth_u64 size = 9000;      /* 9 leaves */
    int i;

    g_sent = g_completed = g_failed = 0;
    g_allow_upload = 1;

    make_source(size, tth);

    CHECK(share_init(&ix) == 0, "share_init failed");
    sf = share_add_file(&ix, "up/LEAVES.ZIP", g_src, size, 0);
    if (!sf) { share_free(&ix); return; }
    base32_decode(raw, sizeof raw, tth);
    share_set_tth(&ix, sf, raw);

    adc_identity_generate(&id_a, 13);
    adc_identity_generate(&id_b, 14);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    /* The serving side is a normal accepted connection. */
    peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);

    /* The asking side is driven by hand: there is no "fetch leaves" entry
       point in the client, because nothing in it wants them yet -- it is
       other clients that ask. */
    peer_accept(&asker, &ta, &id_a, NULL, &cb, 0);

    for (i = 0; i < 200; i++) {
        peer_poll(&asker, 0);
        peer_poll(&server, 0);
        if (asker.state == PEER_IDLE && server.state == PEER_IDLE)
            break;
    }
    CHECK(asker.state == PEER_IDLE, "the handshake did not settle (%s)",
          peer_state_name(asker.state));

    snprintf(line, sizeof line, "CGET tthl TTH/%s 0 -1\n", tth);
    CHECK(tp_write_all(&ta, line, strlen(line)) == 0, "could not ask");

    for (i = 0; i < 2000; i++) {
        peer_poll(&server, 0);
        if (server.state == PEER_SENDING || server.state == PEER_FAILED)
            break;
    }

    /* It must answer with a tthl CSND, not a refusal and not a "file" one:
       a client that reads "CSND file" where it asked for leaves will treat
       the hashes as file content. */
    {
        char got[512];
        int n;
        int saw_csnd = 0;

        for (i = 0; i < 200; i++) {
            n = tp_read(&ta, got, sizeof got - 1);
            if (n > 0) {
                got[n] = '\0';
                if (strstr(got, "CSND tthl TTH/"))
                    saw_csnd = 1;
                if (strstr(got, "CSTA"))
                    CHECK(0, "the leaf request was refused: %s", got);
                break;
            }
            peer_poll(&server, 0);
        }
        CHECK(saw_csnd, "no tthl answer: %s", n > 0 ? got : "(nothing)");
    }

    /* Nine leaves, so 216 bytes. */
    CHECK(server.size == 9 * 24,
          "offered %lu bytes of leaves for 9 leaves",
          (unsigned long)server.size);

    /* And leaves for something we do not have must be refused rather than
       answered with an empty transfer. */
    peer_close(&asker);
    peer_close(&server);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);
    peer_accept(&asker, &ta, &id_a, NULL, &cb, 0);
    for (i = 0; i < 200; i++) { peer_poll(&asker, 0); peer_poll(&server, 0); }

    snprintf(line, sizeof line,
             "CGET tthl TTH/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA 0 -1\n");
    tp_write_all(&ta, line, strlen(line));
    for (i = 0; i < 400; i++)
        peer_poll(&server, 0);
    {
        char got[512];
        int n = tp_read(&ta, got, sizeof got - 1);

        if (n > 0) {
            got[n] = '\0';
            CHECK(strstr(got, "CSTA") != NULL,
                  "leaves for an unknown hash were not refused: %s", got);
        }
    }

    peer_close(&asker);
    peer_close(&server);
    share_free(&ix);
    remove("TTHL.TMP");
}

/* Partial lists, both halves: serving one and asking for one.
 *
 * The share here is two directories deep, because the interesting answers
 * are about what is NOT in a listing -- a server that sent the whole list
 * whatever was asked would pass any test that only checked for the file it
 * asked about.
 */
static void test_partial_list_wire(void)
{
    transport ta, tb;
    pipe_end ea, eb;
    bytepipe ab, ba;
    peerconn asker, server;
    peer_callbacks cb, upcb;
    share_index ix;
    adc_identity id_a, id_b;
    char line[200];
    int i;

    g_sent = g_completed = g_failed = 0;
    g_allow_upload = 1;

    CHECK(share_init(&ix) == 0, "share_init failed");

    /* Hashed, because an unhashed file is left out of any list. The hashes
       are made up: nothing here fetches a file. */
    {
        static const char *const PATHS[] = {
            "up/README.TXT", "up/warez/PKZIP.EXE", "up/warez/doom/DOOM.ZIP"
        };
        size_t k;

        for (k = 0; k < sizeof PATHS / sizeof PATHS[0]; k++) {
            share_file *sf = share_add_file(&ix, PATHS[k], g_src,
                                            1000 + (tth_u64)k, 0);
            unsigned char raw[24];

            if (!sf)
                continue;
            memset(raw, (int)(k + 1), sizeof raw);
            share_set_tth(&ix, sf, raw);
        }
    }

    adc_identity_generate(&id_a, 21);
    adc_identity_generate(&id_b, 22);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);

    memset(&cb, 0, sizeof cb);
    cb.on_complete = on_complete;
    cb.on_failed = on_failed;
    memset(&upcb, 0, sizeof upcb);
    upcb.on_complete = on_sent;
    upcb.on_failed = on_failed;
    upcb.may_upload = may_upload_cb;

    peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);
    peer_accept(&asker, &ta, &id_a, NULL, &cb, 0);

    for (i = 0; i < 200; i++) {
        peer_poll(&asker, 0);
        peer_poll(&server, 0);
        if (asker.state == PEER_IDLE && server.state == PEER_IDLE)
            break;
    }
    CHECK(asker.state == PEER_IDLE, "the handshake did not settle (%s)",
          peer_state_name(asker.state));

    /* ---- a listing of one directory */
    snprintf(line, sizeof line, "CGET list /up/warez/ 0 -1\n");
    CHECK(tp_write_all(&ta, line, strlen(line)) == 0, "could not ask");

    for (i = 0; i < 4000; i++) {
        peer_poll(&server, 0);
        if (server.state == PEER_SENDING || server.state == PEER_FAILED)
            break;
    }

    {
        char got[1024];
        int n, saw = 0;

        for (i = 0; i < 400; i++) {
            n = tp_read(&ta, got, sizeof got - 1);
            if (n > 0) {
                got[n] = '\0';
                if (strstr(got, "CSND list /up/warez/"))
                    saw = 1;
                if (strstr(got, "CSTA"))
                    CHECK(0, "a partial list was refused: %s", got);
                break;
            }
            peer_poll(&server, 0);
        }
        CHECK(saw, "no partial list answer: %s", n > 0 ? got : "(nothing)");
    }

    /* The answer is a real bzip2 file list of that directory and nothing
       else. Read from the staged file rather than the socket: the wire
       check above has already proved it is being sent. */
    {
        bz_reader *r = bz_open(FILELIST_PART_BZ2);
        char xml[4096];
        int n = 0;

        CHECK(r != NULL, "no staged partial list: %s", bz_error());
        if (r) {
            n = bz_read(r, xml, sizeof xml - 1);
            bz_close(&r);
        }
        if (n > 0) {
            xml[n] = '\0';
            CHECK(strstr(xml, "Base=\"/up/warez/\"") != NULL,
                  "the listing does not declare the directory it is of");
            CHECK(strstr(xml, "PKZIP.EXE") != NULL,
                  "the listing is missing the directory's own file");
            CHECK(strstr(xml, "Incomplete=\"1\"") != NULL,
                  "the subdirectory is not marked incomplete");
            CHECK(strstr(xml, "DOOM.ZIP") == NULL,
                  "the listing included a file from a subdirectory");
            CHECK(strstr(xml, "README.TXT") == NULL,
                  "the listing included a file from the level above");
        }
    }

    /* ---- a directory that is not shared is refused, not answered empty */
    peer_close(&asker);
    peer_close(&server);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);
    peer_accept(&asker, &ta, &id_a, NULL, &cb, 0);
    for (i = 0; i < 200; i++) { peer_poll(&asker, 0); peer_poll(&server, 0); }

    snprintf(line, sizeof line, "CGET list /nosuch/ 0 -1\n");
    tp_write_all(&ta, line, strlen(line));
    for (i = 0; i < 2000; i++)
        peer_poll(&server, 0);
    {
        char got[512];
        int n = tp_read(&ta, got, sizeof got - 1);

        if (n > 0) {
            got[n] = '\0';
            CHECK(strstr(got, "CSTA") != NULL,
                  "a listing of an unshared directory was not refused: %s",
                  got);
        } else {
            CHECK(0, "nothing came back for an unshared directory");
        }
    }

    /* ---- recursive, which is what RE1 asks for */
    peer_close(&asker);
    peer_close(&server);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);
    peer_accept(&asker, &ta, &id_a, NULL, &cb, 0);
    for (i = 0; i < 200; i++) { peer_poll(&asker, 0); peer_poll(&server, 0); }

    snprintf(line, sizeof line, "CGET list /up/warez/ 0 -1 RE1\n");
    tp_write_all(&ta, line, strlen(line));
    for (i = 0; i < 4000; i++) {
        peer_poll(&server, 0);
        if (server.state == PEER_SENDING || server.state == PEER_FAILED)
            break;
    }
    {
        bz_reader *r = bz_open(FILELIST_PART_BZ2);
        char xml[4096];
        int n = 0;

        if (r) {
            n = bz_read(r, xml, sizeof xml - 1);
            bz_close(&r);
        }
        if (n > 0) {
            xml[n] = '\0';
            CHECK(strstr(xml, "DOOM.ZIP") != NULL,
                  "RE1 did not bring the subtree");
            CHECK(strstr(xml, "README.TXT") == NULL,
                  "RE1 escaped upwards out of the directory asked for");
        }
    }

    /* ---- and the asking side: the request it sends for a base */
    peer_close(&asker);
    peer_close(&server);
    pipe_pair(&ta, &tb, &ea, &eb, &ab, &ba);
    {
        char dst[384];

        snprintf(dst, sizeof dst, "%s/dosypp_part.bz2", g_dir);
        remove(dst);

        peer_accept(&server, &tb, &id_b, &ix, &upcb, 0);
        CHECK(peer_start_list(&asker, &ta, &id_a, "P1", dst, "/up/warez/",
                              &cb, 0) == 0, "peer_start_list failed");

        for (i = 0; i < 4000; i++) {
            peer_poll(&asker, 0);
            peer_poll(&server, 0);
            if (asker.state == PEER_DONE || asker.state == PEER_FAILED)
                break;
        }

        CHECK(asker.state == PEER_DONE,
              "fetching one directory ended in %s (%s)",
              peer_state_name(asker.state), peer_error(&asker));

        /* What arrived is a listing of that directory -- the same bytes the
           server staged, which the merge side parses. */
        {
            bz_reader *r = bz_open(dst);
            char xml[4096];
            int n = 0;

            CHECK(r != NULL, "nothing arrived: %s", bz_error());
            if (r) {
                n = bz_read(r, xml, sizeof xml - 1);
                bz_close(&r);
            }
            if (n > 0) {
                xml[n] = '\0';
                CHECK(strstr(xml, "Base=\"/up/warez/\"") != NULL,
                      "what arrived is not a listing of what was asked for");
            }
        }
        remove(dst);
    }

    peer_close(&asker);
    peer_close(&server);
    share_free(&ix);
    remove(FILELIST_PART_BZ2);
    remove(FILELIST_PART_XML);
}


/* The rate shown on the transfers door.
 *
 * It used to be moved/(now-started) in whole seconds, which is zero for
 * the whole of a connection's first second -- and a small file on a quick
 * link is finished inside that second, so the row read 0 from beginning to
 * end while the overall counter climbed. Reported from the browser demo,
 * where BLKDROP.ZIP is 109KB.
 *
 * It is a one-second window now, with the bytes moved so far standing in
 * until the first window closes. */
static void test_rate_window(void)
{
    peerconn p;

    memset(&p, 0, sizeof p);

    /* Nothing has moved and no window has closed. */
    p.rate_mark_time = 100;
    CHECK(peer_rate(&p, 100) == 0, "an idle connection has a rate");

    /* Inside the first second, with bytes arriving. The old arithmetic
       returned 0 here, which is the bug. */
    p.moved = 109900;
    CHECK(peer_rate(&p, 100) == 109900,
          "a transfer inside its first second reads %lu, not the bytes it "
          "has moved", peer_rate(&p, 100));

    /* A window has closed: that figure wins, whatever `moved` says. */
    p.rate_last = 320000;
    CHECK(peer_rate(&p, 101) == 320000, "a closed window is not reported");

    /* ...and keeps winning while the next window fills. */
    p.moved = 500000;
    CHECK(peer_rate(&p, 101) == 320000,
          "the last window's rate was replaced by a partial one");

    /* A stall: the window closed with nothing in it. */
    p.rate_last = 0;
    p.rate_mark_moved = p.moved;
    CHECK(peer_rate(&p, 102) == 0, "a stalled transfer still claims a rate");

    /* The second it resumes, there is a figure again rather than a zero. */
    p.moved += 4096;
    CHECK(peer_rate(&p, 102) == 4096, "a resumed transfer reads zero");
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    printf("DOSy++ peer tests (client-to-client transfers)\n");

    snprintf(g_dir, sizeof g_dir, "%s", tmp && tmp[0] ? tmp : "/tmp");
    snprintf(g_src, sizeof g_src, "%s/dosypp_peer_src.bin", g_dir);
    snprintf(g_dst, sizeof g_dst, "%s/dosypp_peer_dst.bin", g_dir);

    /* Small enough to fit one chunk, then large enough to need many and to
       fill the pipe repeatedly. 180000 is what the end-to-end script moves
       between two real clients. */
    test_download(600);
    test_download(8192);        /* exactly one XFER_CHUNK */
    test_download(180000);
    test_unknown_hash();
    test_file_list();
    test_slots();
    test_resume();
    test_resume_corrupt();
    test_inbound_token();
    test_tthl_request();
    test_partial_list_wire();
    test_rate_window();

    remove(g_src);
    remove(g_dst);

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
