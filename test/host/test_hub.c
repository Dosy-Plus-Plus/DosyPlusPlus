/* test_hub.c - SIDs, identity, and the hub state machine.
 *
 * The state machine is driven over a memory transport: hub_feed() pushes the
 * bytes a hub would send, and everything written back lands in a buffer we
 * can assert on. No sockets, no hub, no timing.
 *
 * Password and CID expectations come from RHash + Python, not from our own
 * Tiger -- see test/host/gen_hub_vectors.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/adc/hubconn.h"
#include "../../src/share/index.h"
#include "../../src/adc/ident.h"
#include "../../src/adc/userlist.h"
#include "../../src/adc/search.h"

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

#define CHECK_STR(got, want) do {                                          \
        g_run++;                                                           \
        if (!(got) || strcmp((got), (want)) != 0) {                        \
            printf("FAIL line %d: got \"%s\" want \"%s\"\n",               \
                   __LINE__, (got) ? (got) : "(null)", (want));            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ------------------------------------------------- memory transport */

#define MEMT_CAP 16384

typedef struct {
    char   out[MEMT_CAP];
    size_t outlen;
} memt;

static int memt_read(transport *t, void *buf, size_t len)
{
    (void)t; (void)buf; (void)len;
    return TP_AGAIN;              /* input is pushed with hub_feed */
}

static int memt_write(transport *t, const void *buf, size_t len)
{
    memt *m = (memt *)t->impl;

    if (m->outlen + len > MEMT_CAP)
        return TP_ERROR;
    memcpy(m->out + m->outlen, buf, len);
    m->outlen += len;
    return (int)len;
}

static int memt_pending(transport *t) { (void)t; return 0; }
static void memt_close(transport *t) { t->fd = -1; }

static void memt_init(transport *t, memt *m)
{
    memset(m, 0, sizeof *m);
    memset(t, 0, sizeof *t);
    t->t_read = memt_read;
    t->t_write = memt_write;
    t->t_pending = memt_pending;
    t->t_close = memt_close;
    t->impl = m;
    t->fd = 1;
}

static int sent_contains(memt *m, const char *needle)
{
    m->out[m->outlen < MEMT_CAP ? m->outlen : MEMT_CAP - 1] = '\0';
    return strstr(m->out, needle) != NULL;
}

static void memt_reset(memt *m) { m->outlen = 0; m->out[0] = '\0'; }

/* Feed a line using its actual length. Hardcoding byte counts here cost an
   hour: a count one short dropped the trailing newline, so the message was
   never dispatched and the assertions silently tested nothing. */
static int feed(hubconn *h, const char *s)
{
    return hub_feed(h, s, strlen(s));
}

/* ------------------------------------------------------------- SIDs */

static void test_sid(void)
{
    char buf[8];
    adc_sid sid;

    adc_sid_encode(0, buf);
    CHECK_STR(buf, "AAAA");

    adc_sid_encode(1, buf);
    CHECK_STR(buf, "AAAB");

    /* 20 bits, all set: four 'repeats' of the top alphabet entry. */
    adc_sid_encode(ADC_SID_MAX, buf);
    CHECK_STR(buf, "7777");

    CHECK(adc_sid_decode("AAAA", &sid) == 0 && sid == 0, "AAAA");
    CHECK(adc_sid_decode("AAAB", &sid) == 0 && sid == 1, "AAAB");
    CHECK(adc_sid_decode("7777", &sid) == 0 && sid == ADC_SID_MAX, "7777");

    /* Round-trip every value a hub could assign, cheaply sampled. */
    {
        adc_sid v;
        for (v = 0; v <= ADC_SID_MAX; v += 997) {
            adc_sid back;
            adc_sid_encode(v, buf);
            CHECK(adc_sid_decode(buf, &back) == 0 && back == v,
                  "sid %lu round-trip gave %lu via %s",
                  (unsigned long)v, (unsigned long)back, buf);
        }
    }

    /* Rejections: wrong length and invalid characters. A SID is exactly
       four characters, and accepting "AAA" would silently alias. */
    CHECK(adc_sid_decode("AAA", &sid) == -1, "accepted a 3-char SID");
    CHECK(adc_sid_decode("AAAAA", &sid) == -1, "accepted a 5-char SID");
    CHECK(adc_sid_decode("AA!A", &sid) == -1, "accepted invalid characters");
    CHECK(adc_sid_decode("AA0A", &sid) == -1, "accepted '0', not in base32");
}

/* --------------------------------------------------------- identity */

static void test_identity(void)
{
    adc_identity id;
    int i;

    /* PID = bytes 0x00..0x17, as in test_hash.c; oracle is rhash + Python. */
    for (i = 0; i < ADC_PID_BYTES; i++)
        id.pid[i] = (unsigned char)i;
    adc_identity_derive(&id);

    CHECK_STR(id.pid_b32, "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY");
    CHECK_STR(id.cid_b32, "W6AIUW3CLDF6OGHNVE4JPDDJ2P74IWRCF2O36TA");

    /* Loading a stored PID must reproduce the same CID, or a restart looks
       like a different user to every hub. */
    {
        adc_identity again;
        CHECK(adc_identity_from_b32(&again, id.pid_b32) == 0,
              "could not load the PID we just wrote");
        CHECK_STR(again.cid_b32, id.cid_b32);
        CHECK(memcmp(again.pid, id.pid, ADC_PID_BYTES) == 0, "pid differs");
    }

    /* Garbage must be refused rather than producing a bogus identity. */
    {
        adc_identity bad;
        CHECK(adc_identity_from_b32(&bad, "") == -1, "accepted empty PID");
        CHECK(adc_identity_from_b32(&bad, "AAAA") == -1, "accepted short PID");
        CHECK(adc_identity_from_b32(&bad, "!!!!") == -1, "accepted invalid PID");
    }

    /* Generated identities must at least differ from each other and be
       self-consistent. */
    {
        adc_identity a, b;
        adc_identity check;

        adc_identity_generate(&a, 1);
        adc_identity_generate(&b, 2);
        CHECK(strcmp(a.cid_b32, b.cid_b32) != 0,
              "two generated identities collided");
        CHECK(strlen(a.cid_b32) == 39, "CID is %d chars",
              (int)strlen(a.cid_b32));

        CHECK(adc_identity_from_b32(&check, a.pid_b32) == 0, "reload failed");
        CHECK_STR(check.cid_b32, a.cid_b32);
    }
}

static void test_password(void)
{
    char out[64];

    /* base32(tiger(utf8(password) || decoded_random)), from
       test/host/gen_hub_vectors.sh. The random here decodes to the bytes
       0x00..0x17 -- the same pattern used as the PID elsewhere, which is why
       the empty-password answer below coincides with that PID's CID. That is
       arithmetic, not a copy-paste slip. */
    CHECK(adc_password_response("secret",
              "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY",
              out, sizeof out) == 0, "response failed");
    CHECK_STR(out, "CBHHJOZ6JGOKCXWQWQR3O7UMHDDPWE3FUJPWS7I");

    /* Empty password is legal and must still hash the random. */
    CHECK(adc_password_response("", "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY",
              out, sizeof out) == 0, "empty password failed");
    CHECK_STR(out, "W6AIUW3CLDF6OGHNVE4JPDDJ2P74IWRCF2O36TA");

    /* A challenge that is not base32 must be refused, not hashed as text --
       hashing the encoded form would give a wrong answer that looks exactly
       like a wrong password. */
    CHECK(adc_password_response("secret", "!!!!", out, sizeof out) == -1,
          "accepted a non-base32 challenge");
    CHECK(adc_password_response("secret", "", out, sizeof out) == -1,
          "accepted an empty challenge");

    /* Too small an output buffer must fail rather than overflow. */
    CHECK(adc_password_response("secret", "AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY",
              out, 8) == -1, "wrote into a buffer that was too small");
}

/* --------------------------------------------------------- user list */

static void test_userlist(void)
{
    adc_userlist ul;
    adc_user *u;
    int created;
    unsigned int i;

    adc_userlist_init(&ul);

    CHECK(adc_userlist_find(&ul, 5) == NULL, "found a user in an empty list");

    u = adc_userlist_get(&ul, 5, &created);
    CHECK(u != NULL && created == 1, "insert failed");
    strcpy(u->nick, "alice");
    u->share = 1234;

    u = adc_userlist_get(&ul, 5, &created);
    CHECK(u != NULL && created == 0, "second get should find, not create");
    CHECK_STR(u->nick, "alice");
    CHECK(ul.count == 1, "count is %u, want 1", ul.count);

    /* Many users: exercises index growth and rehashing. Sequential SIDs are
       what real hubs assign, which is the case most likely to cluster. */
    for (i = 100; i < 3000; i++) {
        adc_user *n = adc_userlist_get(&ul, i, NULL);
        CHECK(n != NULL, "insert %u failed", i);
        if (n)
            snprintf(n->nick, sizeof n->nick, "user%u", i);
    }
    CHECK(ul.count == 2901, "count is %u, want 2901", ul.count);

    /* Everything must still be findable after all that growth. */
    for (i = 100; i < 3000; i += 173) {
        char want[32];
        adc_user *f = adc_userlist_find(&ul, i);
        snprintf(want, sizeof want, "user%u", i);
        CHECK(f != NULL, "lost user %u", i);
        if (f)
            CHECK_STR(f->nick, want);
    }
    CHECK(adc_userlist_find(&ul, 5) != NULL, "lost the first user");
    CHECK(adc_userlist_find(&ul, 99) == NULL, "found a user never inserted");

    CHECK(adc_userlist_find_nick(&ul, "ALICE") != NULL,
          "nick lookup is not case-insensitive");
    CHECK(adc_userlist_find_nick(&ul, "nobody") == NULL,
          "found a nick never used");

    /* Removal, then a probe chain that must survive it: a cleared (rather
       than tombstoned) slot would hide users that collided with the removed
       one. */
    CHECK(adc_userlist_remove(&ul, 500) == 1, "remove failed");
    CHECK(adc_userlist_remove(&ul, 500) == 0, "removed twice");
    CHECK(adc_userlist_find(&ul, 500) == NULL, "found a removed user");
    for (i = 100; i < 3000; i += 7)
        if (i != 500)
            CHECK(adc_userlist_find(&ul, i) != NULL,
                  "user %u went missing after an unrelated removal", i);

    /* Reinsertion after removal reuses the hole. */
    u = adc_userlist_get(&ul, 500, &created);
    CHECK(u != NULL && created == 1, "reinsert failed");

    adc_userlist_free(&ul);
}

/* ------------------------------------------------------ state machine */

struct log {
    int   ctm_calls;
    char  ctm_host[32];
    int   ctm_port;
    char  ctm_token[32];
    int   states[16];
    int   nstates;
    char  chat[16][256];
    int   nchat;
    char  pm[8][256];
    int   npm;
    int   joins, parts, updates;
    int   last_sev, last_code;
    char  last_desc[128];
};

static void lg_state(void *user, hub_state s)
{
    struct log *l = (struct log *)user;
    if (l->nstates < 16)
        l->states[l->nstates++] = (int)s;
}
static void lg_chat(void *user, const adc_user *from, const char *text, int me)
{
    struct log *l = (struct log *)user;
    (void)me;
    if (l->nchat < 16)
        snprintf(l->chat[l->nchat++], 256, "%s:%s",
                 from ? from->nick : "(hub)", text);
}
static void lg_pm(void *user, const adc_user *from, const char *text)
{
    struct log *l = (struct log *)user;
    if (l->npm < 8)
        snprintf(l->pm[l->npm++], 256, "%s:%s",
                 from ? from->nick : "?", text);
}
static void lg_join(void *user, const adc_user *u) { (void)u; ((struct log *)user)->joins++; }
static void lg_update(void *user, const adc_user *u) { (void)u; ((struct log *)user)->updates++; }
static void lg_part(void *user, adc_sid sid, const char *nick, const char *why)
{
    (void)sid; (void)nick; (void)why;
    ((struct log *)user)->parts++;
}
static void lg_ctm(void *user, const char *host, int port,
                   const char *token)
{
    struct log *l = (struct log *)user;

    l->ctm_calls++;
    snprintf(l->ctm_host, sizeof l->ctm_host, "%s", host ? host : "");
    l->ctm_port = port;
    snprintf(l->ctm_token, sizeof l->ctm_token, "%s", token ? token : "");
}

static void lg_status(void *user, int sev, int code, const char *desc)
{
    struct log *l = (struct log *)user;
    l->last_sev = sev;
    l->last_code = code;
    snprintf(l->last_desc, sizeof l->last_desc, "%s", desc);
}

static void setup(hubconn *h, transport *t, memt *m, struct log *l,
                  const char *password)
{
    adc_identity id;
    hub_callbacks cb;
    int i;

    for (i = 0; i < ADC_PID_BYTES; i++)
        id.pid[i] = (unsigned char)i;
    adc_identity_derive(&id);

    memset(l, 0, sizeof *l);
    memt_init(t, m);

    memset(&cb, 0, sizeof cb);
    cb.user = l;
    cb.on_state = lg_state;
    cb.on_chat = lg_chat;
    cb.on_private = lg_pm;
    cb.on_user_join = lg_join;
    cb.on_user_update = lg_update;
    cb.on_user_part = lg_part;
    cb.on_status = lg_status;

    hub_init(h, t, "dosypp", password, &id, &cb);
}

static void test_handshake_plain(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    setup(&h, &t, &m, &l, NULL);

    /* HSUP must go out immediately, advertising BASE and TIGR. */
    CHECK(sent_contains(&m, "HSUP ADBASE ADTIGR\n"), "HSUP not sent: %s", m.out);
    CHECK(h.state == HUB_PROTOCOL, "state is %s", hub_state_name(h.state));

    feed(&h, "ISUP ADBASE ADTIGR\n");
    CHECK(h.state == HUB_PROTOCOL, "ISUP alone should not advance state");

    memt_reset(&m);
    feed(&h, "ISID AAAB\n");
    CHECK(h.state == HUB_IDENTIFY, "state is %s", hub_state_name(h.state));
    CHECK(h.sid == 1, "sid is %lu, want 1", (unsigned long)h.sid);

    /* The INF we send must carry our SID positionally, plus ID and PD. */
    CHECK(sent_contains(&m, "BINF AAAB "), "BINF missing or wrong SID: %s", m.out);
    CHECK(sent_contains(&m, "IDW6AIUW3CLDF6OGHNVE4JPDDJ2P74IWRCF2O36TA"),
          "BINF has no/incorrect CID: %s", m.out);
    CHECK(sent_contains(&m, "PDAAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY"),
          "BINF has no/incorrect PID: %s", m.out);
    CHECK(sent_contains(&m, "NIdosypp"), "BINF has no nick: %s", m.out);

    feed(&h, "IINF NIMy\\sHub DEwelcome\n");
    CHECK_STR(h.hubname, "My Hub");

    /* Another user, then ourselves: the echo of our own INF is what marks
       login as complete. */
    feed(&h, "BINF AAAC NIalice SS1024 SL3 VEDC++\\s0.868\n");
    CHECK(h.state == HUB_IDENTIFY, "someone else's INF must not log us in");
    CHECK(l.joins == 1, "joins is %d, want 1", l.joins);

    feed(&h, "BINF AAAB NIdosypp SS0\n");
    CHECK(h.state == HUB_NORMAL, "state is %s", hub_state_name(h.state));
    CHECK(hub_self(&h) != NULL, "hub_self is NULL after our INF echo");

    {
        adc_user *a = adc_userlist_find(&h.users, 2);
        CHECK(a != NULL, "alice missing");
        if (a) {
            CHECK_STR(a->nick, "alice");
            CHECK(a->share == 1024, "share is %lu", (unsigned long)a->share);
            CHECK(a->slots == 3, "slots is %u", a->slots);
            CHECK_STR(a->tag, "DC++ 0.868");
        }
    }

    hub_free(&h);
}

static void test_inf_is_a_partial_update(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;
    adc_user *a;

    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");

    feed(&h, "BINF AAAC NIalice SS2048 DEhello VEDC++\\s1.0\n");
    a = adc_userlist_find(&h.users, 2);
    CHECK(a != NULL && a->share == 2048, "setup failed");

    /* A later INF with only DE must not wipe the nick or the share. This is
       the bug that makes nicks vanish mid-session. */
    feed(&h, "BINF AAAC DEgoodbye\n");
    a = adc_userlist_find(&h.users, 2);
    CHECK(a != NULL, "alice disappeared");
    if (a) {
        CHECK_STR(a->nick, "alice");
        CHECK(a->share == 2048, "share was cleared to %lu",
              (unsigned long)a->share);
        CHECK_STR(a->desc, "goodbye");
        CHECK_STR(a->tag, "DC++ 1.0");
    }
    CHECK(l.updates >= 1, "no update callback");
    CHECK(l.joins == 2, "joins is %d, want 2 (us and alice)", l.joins);

    hub_free(&h);
}

static void test_password_flow(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    setup(&h, &t, &m, &l, "secret");
    feed(&h, "ISID AAAB\n");
    CHECK(h.state == HUB_IDENTIFY, "state is %s", hub_state_name(h.state));

    memt_reset(&m);
    feed(&h, "IGPA AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY\n");
    CHECK(h.state == HUB_VERIFY, "state is %s", hub_state_name(h.state));
    CHECK(sent_contains(&m, "HPAS CBHHJOZ6JGOKCXWQWQR3O7UMHDDPWE3FUJPWS7I"),
          "wrong PAS response: %s", m.out);

    feed(&h, "BINF AAAB NIdosypp\n");
    CHECK(h.state == HUB_NORMAL, "state is %s", hub_state_name(h.state));

    hub_free(&h);
}

static void test_password_missing(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    /* A hub asking for a password we do not have must fail with a clear
       message, not sit in VERIFY forever. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "IGPA AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFY\n");

    CHECK(h.state == HUB_FAILED, "state is %s", hub_state_name(h.state));
    CHECK(strstr(hub_error(&h), "password") != NULL,
          "unhelpful error: %s", hub_error(&h));

    hub_free(&h);
}

static void test_status_and_quit(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    /* Fatal STA: severity 2, code 23 = wrong password. The message must name
       the problem, not just the number. */
    setup(&h, &t, &m, &l, "secret");
    feed(&h, "ISID AAAB\n");
    feed(&h, "ISTA 223 Invalid\\spassword\n");
    CHECK(h.state == HUB_FAILED, "state is %s", hub_state_name(h.state));
    CHECK(l.last_sev == 2 && l.last_code == 23,
          "severity/code parsed as %d/%d", l.last_sev, l.last_code);
    CHECK(strstr(hub_error(&h), "password") != NULL,
          "error does not mention the password: %s", hub_error(&h));
    hub_free(&h);

    /* Recoverable STA must not kill the session. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    feed(&h, "ISTA 100 Just\\ssaying\n");
    CHECK(h.state == HUB_NORMAL, "recoverable status changed state to %s",
          hub_state_name(h.state));
    CHECK(l.last_sev == 1 && l.last_code == 0,
          "parsed %d/%d", l.last_sev, l.last_code);

    /* IQUI for someone else removes them. */
    feed(&h, "BINF AAAC NIalice\n");
    CHECK(h.users.count == 2, "count is %u", h.users.count);
    feed(&h, "IQUI AAAC MSbye\n");
    CHECK(h.users.count == 1, "count is %u after IQUI", h.users.count);
    CHECK(l.parts == 1, "parts is %d", l.parts);

    /* IQUI for US means we have been disconnected. */
    feed(&h, "IQUI AAAB MSkicked\n");
    CHECK(h.state == HUB_FAILED, "state is %s", hub_state_name(h.state));
    CHECK(strstr(hub_error(&h), "kicked") != NULL,
          "error lost the reason: %s", hub_error(&h));

    hub_free(&h);
}

static void test_chat(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    feed(&h, "BINF AAAC NIalice\n");

    feed(&h, "BMSG AAAC hello\\sthere\n");
    CHECK(l.nchat == 1, "nchat is %d", l.nchat);
    if (l.nchat > 0)
        CHECK_STR(l.chat[0], "alice:hello there");

    /* Hub message: no SID, delivered with from == NULL. */
    feed(&h, "IMSG server\\snotice\n");
    CHECK(l.nchat == 2, "nchat is %d", l.nchat);
    if (l.nchat > 1)
        CHECK_STR(l.chat[1], "(hub):server notice");

    /* A D message WITH PM is private... */
    feed(&h, "DMSG AAAC AAAB psst PMAAAC\n");
    CHECK(l.npm == 1, "npm is %d", l.npm);
    if (l.npm > 0)
        CHECK_STR(l.pm[0], "alice:psst");

    /* ...and one WITHOUT PM is still public chat, just addressed. */
    feed(&h, "DMSG AAAC AAAB hi\n");
    CHECK(l.npm == 1, "a D message without PM was treated as private");
    CHECK(l.nchat == 3, "nchat is %d", l.nchat);

    /* Sending. */
    memt_reset(&m);
    CHECK(hub_say(&h, "warez or death") == 0, "say failed: %s", hub_error(&h));
    CHECK(sent_contains(&m, "BMSG AAAB warez\\sor\\sdeath\n"),
          "wrong BMSG: %s", m.out);

    memt_reset(&m);
    CHECK(hub_pm(&h, 2, "hi alice") == 0, "pm failed");
    CHECK(sent_contains(&m, "DMSG AAAB AAAC hi\\salice PMAAAB\n"),
          "wrong DMSG: %s", m.out);

    hub_free(&h);
}

static void test_robustness(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");

    /* Junk must not kill the session: a hub with one buggy bot should not
       disconnect us. */
    feed(&h, "garbage\n");
    feed(&h, "XYZ\n");
    feed(&h, "BINF\n");                   /* no SID */
    feed(&h, "BINF notasid NIbob\n");    /* unusable SID */
    feed(&h, "IQUI\n");                   /* no SID */
    feed(&h, "BMSG AAAZ orphan\n");      /* unknown sender */
    feed(&h, "\n");                       /* keepalive */
    CHECK(h.state == HUB_NORMAL, "junk changed state to %s",
          hub_state_name(h.state));

    /* Commands from later phases must be ignored silently, not treated as
       errors -- hubs send SCH and RES all the time. */
    feed(&h, "BSCH AAAC ANmovie\n");
    feed(&h, "DRES AAAC AAAB FNfile.zip SI123 SL1\n");
    CHECK(h.state == HUB_NORMAL, "a later-phase command broke the session");

    /* A message split across feeds must still arrive intact. */
    l.nchat = 0;
    feed(&h, "BINF AAAC NIalice\n");
    feed(&h, "BMSG AAAC spl");
    CHECK(l.nchat == 0, "delivered an incomplete message");
    feed(&h, "it\\smessage\n");
    CHECK(l.nchat == 1, "nchat is %d", l.nchat);
    if (l.nchat > 0)
        CHECK_STR(l.chat[0], "alice:split message");

    hub_free(&h);
}

/* Answering somebody else's search.
 *
 * This had no unit test at all until AirDC++ asked us for a file and got
 * silence: every check of this path went through two DOSy++ clients, which
 * agree with each other by construction. The lines here are verbatim from
 * what AirDC++ 2.14 actually sent. */
static void test_answers_searches(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;
    share_index ix;
    share_file *f;
    unsigned char tth[TTH_HASH_LEN];

    CHECK(share_init(&ix) == 0, "share_init failed");
    f = share_add_file(&ix, "share/warez/DOSYFILE.ZIP",
                       "/tmp/DOSYFILE.ZIP", 120000, 0);
    CHECK(f != NULL, "share_add_file failed");
    if (!f) { share_free(&ix); return; }
    memset(tth, 0x55, sizeof tth);
    share_set_tth(&ix, f, tth);

    setup(&h, &t, &m, &l, NULL);
    hub_set_share(&h, &ix);
    feed(&h, "ISID AAAE\n");
    feed(&h, "BINF AAAE NIDOSyPP\n");
    feed(&h, "BINF AAAD NIAirPeer\n");

    /* A real client's search. The token is "1/3298537316" -- AirDC++ puts a
       slash in it, which is legal and which a token parser can trip on. */
    memt_reset(&m);
    feed(&h, "BSCH AAAD TO1/3298537316 ANDOSYFILE\n");

    CHECK(sent_contains(&m, "DRES AAAE AAAD"),
          "no result sent to a searching peer: %s", m.out);
    CHECK(sent_contains(&m, "TR"), "the result carries no hash: %s", m.out);
    CHECK(sent_contains(&m, "TO1/3298537316"),
          "the result does not quote the search token: %s", m.out);
    CHECK(sent_contains(&m, "SI120000"), "wrong size: %s", m.out);

    /* A term that matches nothing must produce nothing -- not an empty
       DRES, which some clients treat as a result. */
    memt_reset(&m);
    feed(&h, "BSCH AAAD TOX2 ANNOTHINGLIKETHIS\n");
    CHECK(!sent_contains(&m, "DRES"),
          "answered a search that matches nothing: %s", m.out);

    /* Our own search coming back as a broadcast must not be answered. */
    memt_reset(&m);
    feed(&h, "BSCH AAAE TOD1 ANDOSYFILE\n");
    CHECK(!sent_contains(&m, "DRES"), "answered our own search: %s", m.out);

    /* A hash search for what we have, which is how a resumed download finds
       a source. */
    {
        char line[160];
        char b32[BASE32_TIGER_LEN + 1];

        base32_encode(b32, sizeof b32, tth, TTH_HASH_LEN);
        snprintf(line, sizeof line, "BSCH AAAD TOX3 TR%s\n", b32);
        memt_reset(&m);
        feed(&h, line);
        CHECK(sent_contains(&m, "DRES AAAE AAAD"),
              "did not answer a search by hash: %s", m.out);
    }

    /* Free slots are advertised as they change: other clients read SL to
       decide whether asking us is worth it. Checked here rather than in an
       end-to-end run, where a transfer over loopback finishes inside one
       tick and the number is never seen to move. */
    memt_reset(&m);
    CHECK(hub_update_slots(&h, 0) == 0, "could not update slots");
    CHECK(sent_contains(&m, "BINF AAAE SL0"),
          "wrong slot update: %s", m.out);
    memt_reset(&m);
    CHECK(hub_update_slots(&h, 3) == 0, "could not update slots");
    CHECK(sent_contains(&m, "BINF AAAE SL3"),
          "wrong slot update: %s", m.out);

    /* And with no share attached at all, silence rather than a crash. */
    hub_set_share(&h, NULL);
    memt_reset(&m);
    feed(&h, "BSCH AAAD TOX4 ANDOSYFILE\n");
    CHECK(!sent_contains(&m, "DRES"),
          "answered a search with no share: %s", m.out);

    hub_free(&h);
    share_free(&ix);
}

static void test_keepalive_and_death(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;
    unsigned long now = 1000000UL;

    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    CHECK(h.state == HUB_NORMAL, "setup: state is %s", hub_state_name(h.state));

    /* First tick only establishes a baseline. Without that, timestamps left
       at zero would read as "silent since 1970" and fail at once. */
    CHECK(hub_tick(&h, now) == 0, "first tick failed");
    memt_reset(&m);

    /* Nothing due yet. */
    CHECK(hub_tick(&h, now + HUB_KEEPALIVE_SECS - 1) == 0, "tick failed");
    CHECK(!sent_contains(&m, "\n"), "sent a keepalive too early: %s", m.out);

    /* Due: an empty line, which ADC defines as a no-op. */
    CHECK(hub_tick(&h, now + HUB_KEEPALIVE_SECS) == 0, "tick failed");
    CHECK(m.outlen == 1 && m.out[0] == '\n',
          "keepalive was %d bytes, want exactly one newline", (int)m.outlen);

    /* Having just sent one, another must not follow immediately. */
    memt_reset(&m);
    CHECK(hub_tick(&h, now + HUB_KEEPALIVE_SECS + 1) == 0, "tick failed");
    CHECK(m.outlen == 0, "sent a second keepalive straight away");

    /* Traffic from us also defers the keepalive. */
    now += HUB_KEEPALIVE_SECS + 1;
    hub_say(&h, "hello");
    memt_reset(&m);
    CHECK(hub_tick(&h, now + 5) == 0, "tick failed");
    CHECK(m.outlen == 0, "chatting did not defer the keepalive");

    /* Silence from the hub for long enough is fatal, and the message must
       say what happened rather than just "connection lost". */
    CHECK(hub_tick(&h, now + HUB_DEAD_SECS) == -1,
          "a dead hub was not detected");
    CHECK(h.state == HUB_FAILED, "state is %s", hub_state_name(h.state));
    CHECK(strstr(hub_error(&h), "stopped responding") != NULL,
          "unhelpful error: %s", hub_error(&h));
    hub_free(&h);

    /* Anything arriving resets the clock -- including a bare keepalive
       newline from the hub, which parses to no message at all. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    hub_tick(&h, now);
    hub_tick(&h, now + HUB_DEAD_SECS - 10);
    feed(&h, "\n");                       /* hub's own keepalive */
    CHECK(hub_tick(&h, now + HUB_DEAD_SECS - 5) == 0,
          "a hub keepalive did not count as life");
    CHECK(h.state == HUB_NORMAL, "state is %s", hub_state_name(h.state));
    hub_free(&h);

    /* Liveness is not policed during login: a hub may take its time, and
       main() has its own handshake timeout. */
    setup(&h, &t, &m, &l, NULL);
    hub_tick(&h, now);
    CHECK(hub_tick(&h, now + HUB_DEAD_SECS * 2) == 0,
          "killed a connection that was still logging in");
    CHECK(h.state == HUB_PROTOCOL, "state is %s", hub_state_name(h.state));
    hub_free(&h);

    /* Disabling both knobs must disable both behaviours. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    h.keepalive_secs = 0;
    h.dead_secs = 0;
    hub_tick(&h, now);
    memt_reset(&m);
    CHECK(hub_tick(&h, now + 100000UL) == 0, "disabled checks still fired");
    CHECK(m.outlen == 0, "sent a keepalive with keepalive_secs = 0");
    hub_free(&h);
}

static void test_say_before_login(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    /* Chatting before NORMAL must be refused locally rather than sent --
       a hub would answer ISTA 44 "invalid state" and might drop us. */
    setup(&h, &t, &m, &l, NULL);
    memt_reset(&m);
    CHECK(hub_say(&h, "too early") == -1, "say succeeded before login");
    CHECK(!sent_contains(&m, "BMSG"), "sent a BMSG before login: %s", m.out);
    CHECK(hub_pm(&h, 2, "too early") == -1, "pm succeeded before login");

    hub_free(&h);
}

/* -------------------------------------------------------------- search */

static void test_search_build(void)
{
    search_state ss;
    search_query q;
    char out[ADC_LINE_MAX];

    search_init(&ss);

    /* An empty query must be refused rather than asking a hub for
       everything, which gets you a flood or a kick. */
    memset(&q, 0, sizeof q);
    CHECK(search_build(&ss, 1, &q, out, sizeof out, 0) == -1,
          "built a search with no terms");

    /* A phrase becomes one AN field per word: a hub matches each AN
       independently, so sending "doom shareware" as a single field would
       only match that exact substring. */
    snprintf(q.terms, sizeof q.terms, "doom shareware");
    CHECK(search_build(&ss, 1, &q, out, sizeof out, 100) == 0, "build failed");
    CHECK(strstr(out, "BSCH AAAB") == out, "wrong prefix: %s", out);
    CHECK(strstr(out, "ANdoom") != NULL, "no ANdoom: %s", out);
    CHECK(strstr(out, "ANshareware") != NULL, "no ANshareware: %s", out);
    CHECK(strstr(out, "TO") != NULL, "no token: %s", out);
    CHECK(ss.active == 1, "search not marked active");
    CHECK(ss.started == 100, "started is %lu", ss.started);

    /* Each search gets a fresh token, so late results from the previous one
       can be told apart. */
    {
        char first[16];
        snprintf(first, sizeof first, "%s", ss.token);
        search_build(&ss, 1, &q, out, sizeof out, 101);
        CHECK(strcmp(first, ss.token) != 0, "token was reused");
    }

    /* Exclusions, group and size bounds. */
    memset(&q, 0, sizeof q);
    snprintf(q.terms, sizeof q.terms, "doom");
    snprintf(q.exclude, sizeof q.exclude, "demo");
    q.group = SEARCH_ARCHIVE;
    q.min_size = 1024;
    q.max_size = 1048576;
    CHECK(search_build(&ss, 1, &q, out, sizeof out, 0) == 0, "build failed");
    CHECK(strstr(out, "NOdemo") != NULL, "no exclusion: %s", out);
    CHECK(strstr(out, "GR2") != NULL, "no group: %s", out);
    CHECK(strstr(out, "GE1024") != NULL, "no minimum: %s", out);
    CHECK(strstr(out, "LE1048576") != NULL, "no maximum: %s", out);

    /* A hash search sends TR and nothing else -- adding terms would narrow
       an exact match wrongly. */
    memset(&q, 0, sizeof q);
    q.group = SEARCH_TTH;
    snprintf(q.tth, sizeof q.tth, "QAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII");
    snprintf(q.terms, sizeof q.terms, "ignored");
    CHECK(search_build(&ss, 1, &q, out, sizeof out, 0) == 0, "build failed");
    CHECK(strstr(out, "TRQAHL3II3") != NULL, "no TTH: %s", out);
    CHECK(strstr(out, "ANignored") == NULL,
          "a hash search also sent terms: %s", out);

    /* A hash search with no hash is empty. */
    memset(&q, 0, sizeof q);
    q.group = SEARCH_TTH;
    CHECK(search_build(&ss, 1, &q, out, sizeof out, 0) == -1,
          "built a hash search with no hash");

    /* Spaces in terms must be escaped, not sent raw -- a raw space would
       split one term into two fields. */
    memset(&q, 0, sizeof q);
    snprintf(q.terms, sizeof q.terms, "two words");
    search_build(&ss, 1, &q, out, sizeof out, 0);
    CHECK(strchr(out, '\n') == NULL, "the line contains a newline");
}

static void feed_res(search_state *ss, const char *line)
{
    adc_msg m;

    CHECK(adc_parse(&m, line) == 0, "could not parse %s", line);
    search_add_result(ss, &m);
}

static void test_search_results(void)
{
    search_state ss;
    search_query q;
    char out[ADC_LINE_MAX];

    search_init(&ss);
    memset(&q, 0, sizeof q);
    snprintf(q.terms, sizeof q.terms, "doom");
    search_build(&ss, 1, &q, out, sizeof out, 0);

    /* The same file from three peers is ONE result with three sources.
       Without coalescing, a popular file fills the screen with one name. */
    feed_res(&ss, "DRES AAAC AAAB FNwarez/DOOM.ZIP SI1000 SL2 "
                  "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TOD1");
    feed_res(&ss, "DRES AAAD AAAB FNstuff/DOOM.ZIP SI1000 SL1 "
                  "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TOD1");
    feed_res(&ss, "DRES AAAE AAAB FNelse/DOOM.ZIP SI1000 SL3 "
                  "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TOD1");
    CHECK(ss.count == 1, "count is %d, want 1 (coalesced)", ss.count);
    CHECK(ss.results[0].nsource == 3, "nsource is %d, want 3",
          ss.results[0].nsource);
    CHECK_STR(ss.results[0].name, "DOOM.ZIP");
    CHECK(ss.results[0].size == 1000, "size is %lu",
          (unsigned long)ss.results[0].size);

    /* The same peer answering twice (TCP and UDP for one search) is one
       source, not two. */
    feed_res(&ss, "DRES AAAC AAAB FNwarez/DOOM.ZIP SI1000 SL2 "
                  "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TOD1");
    CHECK(ss.results[0].nsource == 3, "a repeat became a new source");

    /* A different hash is a different result even with the same name. */
    feed_res(&ss, "DRES AAAC AAAB FNother/DOOM.ZIP SI2000 SL1 "
                  "TRKEUMTH2Y4MQGVBCZHNRI6OL7QKKGANBQR2IK7GI TOD1");
    CHECK(ss.count == 2, "count is %d, want 2", ss.count);

    /* Results for a stale token are discarded: a slow peer can answer after
       the user has searched for something else. */
    feed_res(&ss, "DRES AAAC AAAB FNold/STALE.ZIP SI1 SL1 "
                  "TRULXIYNB3X47MD5MC6XVVIC4GT5DGJV5QPTHXAKQ TOWRONG");
    CHECK(ss.count == 2, "a stale-token result was accepted");

    /* A directory has no size and no hash, and is matched by name. */
    feed_res(&ss, "DRES AAAC AAAB FNwarez/incoming TOD1");
    CHECK(ss.count == 3, "directory result not added");
    CHECK(ss.results[2].is_dir == 1, "directory not flagged");
    feed_res(&ss, "DRES AAAD AAAB FNelsewhere/incoming TOD1");
    CHECK(ss.count == 3, "directories with the same name did not coalesce");
    CHECK(ss.results[2].nsource == 2, "directory has %d sources",
          ss.results[2].nsource);

    /* Malformed results are ignored, not fatal. */
    feed_res(&ss, "DRES AAAC AAAB TOD1");             /* no FN */
    CHECK(ss.count == 3, "a result with no filename was accepted");

    /* The table is bounded: a popular search on a busy hub must not grow
       until a 16MB machine dies. */
    {
        int i;
        for (i = 0; i < SR_MAX_RESULTS + 50; i++) {
            char line[256];
            snprintf(line, sizeof line,
                     "DRES AAAC AAAB FNbulk/f%d.bin SI%d SL1 TRZZZ%034d TOD1",
                     i, i + 1, i);
            feed_res(&ss, line);
        }
        CHECK(ss.count == SR_MAX_RESULTS, "count is %d, cap is %d",
              ss.count, SR_MAX_RESULTS);
        CHECK(ss.dropped > 0, "nothing was reported as dropped");
    }

    /* Sources per result are bounded too. */
    search_init(&ss);
    search_build(&ss, 1, &q, out, sizeof out, 0);
    {
        int i;
        for (i = 0; i < SR_MAX_SOURCES + 10; i++) {
            char line[256];
            snprintf(line, sizeof line,
                     "DRES %s AAAB FNx/F.ZIP SI5 SL1 "
                     "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TO%s",
                     (i % 2) ? "AAAC" : "AAAD", ss.token);
            /* Distinct SIDs so each is a new source. */
            line[5] = (char)('A' + (i % 26));
            line[6] = (char)('A' + ((i / 26) % 26));
            feed_res(&ss, line);
        }
        CHECK(ss.results[0].nsource <= SR_MAX_SOURCES,
              "nsource is %d, cap is %d", ss.results[0].nsource,
              SR_MAX_SOURCES);
    }
}

static void test_search_sort_and_tag(void)
{
    search_state ss;
    search_query q;
    char out[ADC_LINE_MAX];

    search_init(&ss);
    memset(&q, 0, sizeof q);
    snprintf(q.terms, sizeof q.terms, "x");
    search_build(&ss, 1, &q, out, sizeof out, 0);

    feed_res(&ss, "DRES AAAC AAAB FNa/BIG.ZIP SI9000 SL1 TRAAA1 TOD1");
    feed_res(&ss, "DRES AAAC AAAB FNa/small.zip SI10 SL1 TRAAA2 TOD1");
    feed_res(&ss, "DRES AAAC AAAB FNa/MID.ZIP SI500 SL1 TRAAA3 TOD1");
    feed_res(&ss, "DRES AAAD AAAB FNa/MID.ZIP SI500 SL1 TRAAA3 TOD1");
    CHECK(ss.count == 3, "count is %d", ss.count);

    search_sort_by(&ss, SORT_SIZE);
    CHECK_STR(ss.results[0].name, "BIG.ZIP");

    search_sort_by(&ss, SORT_NAME);
    /* Case-insensitive, so BIG / MID / small sort together rather than
       upper-case first. */
    CHECK_STR(ss.results[0].name, "BIG.ZIP");
    CHECK_STR(ss.results[2].name, "small.zip");

    search_sort_by(&ss, SORT_SOURCES);
    CHECK(ss.results[0].nsource == 2,
          "most-sourced result is not first (nsource %d)",
          ss.results[0].nsource);

    /* Tagging. */
    CHECK(search_tagged_count(&ss) == 0, "something was pre-tagged");
    search_tag_toggle(&ss, 0);
    CHECK(search_tagged_count(&ss) == 1, "tag did not take");
    search_tag_toggle(&ss, 0);
    CHECK(search_tagged_count(&ss) == 0, "untag did not take");
    search_tag_all(&ss, 1);
    CHECK(search_tagged_count(&ss) == 3, "tag-all missed some");
    search_tag_all(&ss, 0);
    CHECK(search_tagged_count(&ss) == 0, "untag-all missed some");

    /* Out-of-range indices must be ignored, not written through. */
    search_tag_toggle(&ss, -1);
    search_tag_toggle(&ss, 9999);
    CHECK(search_tagged_count(&ss) == 0, "an out-of-range tag took effect");

    /* Sizes read the way a BBS showed them. */
    {
        char buf[16];
        search_fmt_size(0, buf, sizeof buf);
        CHECK_STR(buf, "0B");
        search_fmt_size(1023, buf, sizeof buf);
        CHECK_STR(buf, "1023B");
        search_fmt_size(1024, buf, sizeof buf);
        CHECK_STR(buf, "1.0K");
        search_fmt_size(2387968, buf, sizeof buf);
        CHECK_STR(buf, "2.2M");
    }
}

static void test_search_via_hub(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;
    search_query q;

    setup(&h, &t, &m, &l, NULL);

    /* Searching before login must be refused locally rather than sent. */
    memset(&q, 0, sizeof q);
    snprintf(q.terms, sizeof q.terms, "doom");
    CHECK(hub_search(&h, &q, 0) == -1, "searched before logging in");

    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIdosypp\n");
    feed(&h, "BINF AAAC NIalice\n");

    memt_reset(&m);
    CHECK(hub_search(&h, &q, 50) == 0, "search failed: %s", hub_error(&h));
    CHECK(sent_contains(&m, "BSCH AAAB"), "wrong search line: %s", m.out);
    CHECK(sent_contains(&m, "ANdoom"), "no term: %s", m.out);

    /* A result addressed to us is accepted and surfaces through the
       callback path into h->search. */
    {
        char line[256];
        snprintf(line, sizeof line,
                 "DRES AAAC AAAB FNwarez/DOOM.ZIP SI1234 SL2 "
                 "TRQAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII TO%s\n",
                 h.search.token);
        feed(&h, line);
    }
    CHECK(h.search.count == 1, "count is %d", h.search.count);
    CHECK_STR(h.search.results[0].name, "DOOM.ZIP");
    CHECK(h.search.results[0].nsource == 1, "nsource is %d",
          h.search.results[0].nsource);
    CHECK(h.search.results[0].sources[0].sid == 2,
          "source sid is %lu, want 2",
          (unsigned long)h.search.results[0].sources[0].sid);

    /* Searches from OTHER users (BSCH arriving at us) must not be mistaken
       for results, and must not disturb our own search state. */
    feed(&h, "BSCH AAAC ANsomething\n");
    CHECK(h.search.count == 1, "another user's search changed our results");
    CHECK(h.state == HUB_NORMAL, "a BSCH broke the session");

    hub_free(&h);
}

/* ------------------------------------------------- connection setup */

static void test_connect_requests(void)
{
    hubconn h;
    transport t;
    memt m;
    struct log l;

    /* Case 1: we are PASSIVE, they are ACTIVE. We must send RCM asking
       them to dial us. This is the common case for a DOS client behind
       anything. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIthem SUADC0,TCP4 I410.0.0.5\n");

    {
        adc_user *u = adc_userlist_find(&h.users, 2);
        CHECK(u != NULL, "peer missing");
        if (u) {
            CHECK(u->passive == 0, "a peer advertising TCP4 was read as "
                                   "passive");
            CHECK_STR(u->ip, "10.0.0.5");
        }
    }

    memt_reset(&m);
    CHECK(hub_request_connection(&h, 2, "T1") == 0,
          "passive->active request refused: %s", hub_error(&h));
    CHECK(sent_contains(&m, "DRCM AAAB AAAC ADC/1.0 T1"),
          "wrong RCM: %s", m.out);
    hub_free(&h);

    /* Case 2: we are ACTIVE. We send CTM with our port, whatever they are. */
    setup(&h, &t, &m, &l, NULL);
    hub_set_active(&h, 1, 1412, "0.0.0.0");
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIthem SUADC0\n");

    memt_reset(&m);
    CHECK(hub_request_connection(&h, 2, "T2") == 0, "active request failed");
    CHECK(sent_contains(&m, "DCTM AAAB AAAC ADC/1.0 1412 T2"),
          "wrong CTM: %s", m.out);
    hub_free(&h);

    /* Case 3: both passive. No connection is possible and it must be
       reported as such rather than retried for ever. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIthem SUADC0\n");

    {
        adc_user *u = adc_userlist_find(&h.users, 2);
        CHECK(u && u->passive == 1, "a peer without TCP4 was read as active");
    }

    memt_reset(&m);
    CHECK(hub_request_connection(&h, 2, "T3") == -1,
          "a passive-to-passive request was allowed");
    CHECK(strstr(hub_error(&h), "passive") != NULL,
          "unhelpful error: %s", hub_error(&h));
    hub_free(&h);

    /* Case 4: our INF must advertise TCP4 only when we are active -- that
       field is how peers decide whether a CTM to us is worth sending. */
    setup(&h, &t, &m, &l, NULL);
    hub_set_active(&h, 1, 1412, "0.0.0.0");
    memt_reset(&m);
    feed(&h, "ISID AAAB\n");
    CHECK(sent_contains(&m, "TCP4"), "active INF has no TCP4: %s", m.out);
    hub_free(&h);

    setup(&h, &t, &m, &l, NULL);
    memt_reset(&m);
    feed(&h, "ISID AAAB\n");
    CHECK(!sent_contains(&m, "TCP4"), "passive INF claims TCP4: %s", m.out);
    /* And never ADC0: that announces encrypted client-to-client support,
       which we do not have. uhub strips it and warns; a hub that took us at
       our word would arrange ADCS transfers we cannot speak. */
    CHECK(!sent_contains(&m, "ADC0"),
          "INF claims ADC0 (TLS c2c) support we do not have: %s", m.out);
    hub_free(&h);

    /* A user we have only just heard of must count as passive: being
       reachable is a claim (TCP4), and assuming it sends CTMs into
       nowhere. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIquiet\n");       /* no SU at all */
    {
        adc_user *u = adc_userlist_find(&h.users, 2);
        CHECK(u != NULL, "peer missing");
        CHECK(u && u->passive == 1,
              "a peer that advertised no SU was assumed reachable");
    }
    CHECK(hub_request_connection(&h, 2, "T9") == -1,
          "asked an unreachable peer for a connection anyway");
    hub_free(&h);

    /* Case 5: an incoming CTM must be acted on, and the address must come
       from the user list rather than the message -- a peer does not get to
       name an arbitrary host for us to dial. */
    setup(&h, &t, &m, &l, NULL);
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIthem SUADC0,TCP4 I410.0.0.9\n");
    hub_set_ctm_handler(&h, lg_ctm);
    h.cb.user = &l;
    feed(&h, "DCTM AAAC AAAB ADC/1.0 4000 T9\n");
    CHECK(l.ctm_calls == 1, "CTM handler called %d times", l.ctm_calls);
    CHECK_STR(l.ctm_host, "10.0.0.9");
    CHECK(l.ctm_port == 4000, "port is %d", l.ctm_port);
    CHECK_STR(l.ctm_token, "T9");

    /* An unsupported protocol must be refused, not dialled. */
    l.ctm_calls = 0;
    memt_reset(&m);
    feed(&h, "DCTM AAAC AAAB ADCS/0.10 4001 TA\n");
    CHECK(l.ctm_calls == 0, "dialled an ADCS peer we cannot talk to");
    CHECK(sent_contains(&m, "DSTA"), "did not report the refusal: %s",
          m.out);
    hub_free(&h);

    /* Case 6: an RCM from a passive peer, when we are active, must be
       answered with a CTM. */
    setup(&h, &t, &m, &l, NULL);
    hub_set_active(&h, 1, 1412, "0.0.0.0");
    feed(&h, "ISID AAAB\n");
    feed(&h, "BINF AAAB NIme\n");
    feed(&h, "BINF AAAC NIthem SUADC0\n");
    memt_reset(&m);
    feed(&h, "DRCM AAAC AAAB ADC/1.0 TB\n");
    CHECK(sent_contains(&m, "DCTM AAAB AAAC ADC/1.0 1412 TB"),
          "did not answer an RCM with a CTM: %s", m.out);
    hub_free(&h);
}

int main(void)
{
    printf("DOSy++ hub tests (SID / identity / state machine / search)\n");

    test_sid();
    test_identity();
    test_password();
    test_userlist();

    test_handshake_plain();
    test_inf_is_a_partial_update();
    test_password_flow();
    test_password_missing();
    test_status_and_quit();
    test_chat();
    test_robustness();
    test_keepalive_and_death();
    test_say_before_login();
    test_search_build();
    test_search_results();
    test_search_sort_and_tag();
    test_search_via_hub();
    test_connect_requests();
    test_answers_searches();

    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
