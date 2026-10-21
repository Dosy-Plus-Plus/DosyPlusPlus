/* hubconn.c - hub session state machine. */
#include "hubconn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Features we claim in SUP. BASE is mandatory; TIGR says we can hash, which
   the hub needs to know before it can ask us for a password. */
#define SUP_FEATURES "ADBASE ADTIGR"

static void set_err(hubconn *h, const char *fmt, const char *a, int n)
{
    if (a && n >= 0)
        snprintf(h->err, sizeof h->err, fmt, a, n);
    else if (a)
        snprintf(h->err, sizeof h->err, fmt, a);
    else
        snprintf(h->err, sizeof h->err, "%s", fmt);
}

static void goto_state(hubconn *h, hub_state s)
{
    if (h->state == s)
        return;
    h->state = s;
    if (h->cb.on_state)
        h->cb.on_state(h->cb.user, s);
}

static int send_line(hubconn *h, const char *line)
{
    size_t len = strlen(line);
    char buf[ADC_LINE_MAX];

    if (len + 2 > sizeof buf) {
        set_err(h, "outgoing message too long", NULL, -1);
        return -1;
    }

    memcpy(buf, line, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    if (tp_write_all(h->t, buf, len + 1) != 0) {
        set_err(h, "write failed", NULL, -1);
        return -1;
    }

    h->last_tx = h->now;
    return 0;
}

/* ------------------------------------------------------------------ INF */

static unsigned long parse_ul(const char *s)
{
    return s ? strtoul(s, NULL, 10) : 0;
}

/* Saturating, shared with the search code: a share size is a field from
   the network, and one that wrapped would be displayed and sorted on. Two
   hand-rolled copies of this existed; strtoull is not reliably present in
   DJGPP's gnu89 headers, which is why it is hand-rolled at all. */
#define parse_u64(s) adc_u64_parse(s)

static void copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!src)
        return;
    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Merge an INF into a user record.
 *
 * INF is a partial update: a user changing only their description sends only
 * DE. So absent fields must be left alone, never cleared -- otherwise nicks
 * disappear from the list the first time anyone changes anything. */
static void merge_inf(adc_user *u, const adc_msg *m)
{
    const char *v;

    if ((v = adc_field(m, "NI")) != NULL)
        copy_field(u->nick, sizeof u->nick, v);
    if ((v = adc_field(m, "DE")) != NULL)
        copy_field(u->desc, sizeof u->desc, v);
    if ((v = adc_field(m, "ID")) != NULL)
        copy_field(u->cid, sizeof u->cid, v);
    if ((v = adc_field(m, "SS")) != NULL)
        u->share = parse_u64(v);
    if ((v = adc_field(m, "SF")) != NULL)
        u->files = parse_ul(v);
    if ((v = adc_field(m, "SL")) != NULL)
        u->slots = (unsigned int)parse_ul(v);
    if ((v = adc_field(m, "HN")) != NULL)
        u->hubs_normal = (unsigned int)parse_ul(v);
    if ((v = adc_field(m, "HR")) != NULL)
        u->hubs_reg = (unsigned int)parse_ul(v);
    if ((v = adc_field(m, "HO")) != NULL)
        u->hubs_op = (unsigned int)parse_ul(v);
    if ((v = adc_field(m, "CT")) != NULL)
        u->ct = (unsigned int)parse_ul(v);

    /* SU tells us whether they can be dialled. Without TCP4 they are
       passive, and a CTM to them would go nowhere. */
    if ((v = adc_field(m, "I4")) != NULL && strcmp(v, "0.0.0.0") != 0)
        copy_field(u->ip, sizeof u->ip, v);
    if ((v = adc_field(m, "SU")) != NULL) {
        copy_field(u->su, sizeof u->su, v);
        u->passive = (strstr(v, "TCP4") == NULL) ? 1 : 0;
    }

    /* Client tag. Newer clients split application name (AP) from version
       (VE); older ones put "name version" in VE alone. Present both the
       same way so the user list does not look inconsistent. */
    {
        const char *ap = adc_field(m, "AP");
        const char *ve = adc_field(m, "VE");

        if (ap && ve)
            snprintf(u->tag, sizeof u->tag, "%s %s", ap, ve);
        else if (ve)
            copy_field(u->tag, sizeof u->tag, ve);
        else if (ap)
            copy_field(u->tag, sizeof u->tag, ap);
    }
}

/* Only hashed files count. Reporting bytes we cannot serve by hash would
   get us into hubs on false pretences and then fail every request. */
static tiger_u64 share_bytes(const hubconn *h)
{
    return h->share ? h->share->hashed_size : 0;
}

static unsigned int share_files(const hubconn *h)
{
    return h->share ? h->share->hashed_count : 0;
}

static int send_own_inf(hubconn *h)
{
    char line[ADC_LINE_MAX];
    char sid_text[ADC_SID_CHARS + 1];
    char nick_esc[ADC_NICK_MAX * 2 + 4];
    char desc_esc[ADC_DESC_MAX * 2 + 4];
    char ss_text[24];

    adc_sid_encode(h->sid, sid_text);
    adc_u64_dec(share_bytes(h), ss_text, sizeof ss_text);

    if (adc_escape(nick_esc, sizeof nick_esc, h->nick) < 0) {
        set_err(h, "nick does not fit after escaping", NULL, -1);
        return -1;
    }
    if (adc_escape(desc_esc, sizeof desc_esc, h->desc) < 0)
        desc_esc[0] = '\0';

    /* PD (the PID) is sent only during login: the hub checks
       tiger(PID) == CID and then discards it. Sending it later would leak
       the one secret that proves this identity is ours. */
    /* SU lists what we support. TCP4 is the claim that we can be dialled;
       omitting it is how a passive client declares itself, and peers use it
       to decide whether a CTM to us is worth sending.
     *
     * Not ADC0: that flag means "I accept encrypted client-to-client
     * connections", and we have no TLS at all (TODO.md). uhub strips it
     * with a warning, but a hub that believed it would arrange ADCS
     * transfers we cannot speak. So a passive client advertises nothing
     * and the SU field is left out entirely. */
    snprintf(line, sizeof line,
             "BINF %s ID%s PD%s NI%s SL%u SS%s SF%u HN1 HR0 HO0 "
             "VEDOSy++\\s" DOSYPP_VERSION "%s%s%s%s",
             sid_text, h->id.cid_b32, h->id.pid_b32, nick_esc, h->slots,
             ss_text, share_files(h),
             h->active ? " SUTCP4 I4" : "",
             h->active ? h->local_ip : "",
             desc_esc[0] ? " DE" : "", desc_esc);

    return send_line(h, line);
}

void hub_set_share(hubconn *h, share_index *ix)
{
    h->share = ix;
}

void hub_set_active(hubconn *h, int active, int port, const char *ip)
{
    h->active = active ? 1 : 0;
    h->listen_port = port;
    copy_field(h->local_ip, sizeof h->local_ip, ip ? ip : "0.0.0.0");
}

void hub_set_ctm_handler(hubconn *h, hub_ctm_fn fn)
{
    h->on_ctm = fn;
}

int hub_request_connection(hubconn *h, adc_sid to, const char *token)
{
    char line[ADC_LINE_MAX];
    char me[ADC_SID_CHARS + 1];
    char them[ADC_SID_CHARS + 1];
    char tok[64];

    if (h->state != HUB_NORMAL) {
        set_err(h, "not logged in yet", NULL, -1);
        return -1;
    }
    if (adc_escape(tok, sizeof tok, token) < 0)
        return -1;

    adc_sid_encode(h->sid, me);
    adc_sid_encode(to, them);

    if (h->active) {
        /* We can be dialled, so say where. ADC/1.0 is the protocol name
           for a plain client-to-client connection; ADCS/0.10 would be the
           TLS one, which we do not offer. */
        snprintf(line, sizeof line, "DCTM %s %s ADC/1.0 %d %s",
                 me, them, h->listen_port, tok);
        return send_line(h, line);
    }

    /* We cannot be dialled. Ask them to tell us where to dial instead --
       which only works if THEY are active. If they are passive too this
       goes unanswered, and the caller moves on to another source. */
    {
        adc_user *u = adc_userlist_find(&h->users, to);

        if (!u) {
            /* We have no record of them at all, which means we never saw
               their INF. Asking anyway is pointless. */
            snprintf(h->err, sizeof h->err,
                     "no record of that node; cannot ask for a connection");
            return -1;
        }
        if (u->passive) {
            /* Name them and quote what they advertised: "both sides are
               passive" on its own gives nobody anything to act on. */
            snprintf(h->err, sizeof h->err,
                     "we are passive and so is %s (SU=%s) -- "
                     "set active=yes to download from them",
                     u->nick[0] ? u->nick : "that node",
                     u->su[0] ? u->su : "not advertised");
            return -1;
        }
    }

    snprintf(line, sizeof line, "DRCM %s %s ADC/1.0 %s", me, them, tok);
    return send_line(h, line);
}

int hub_update_share(hubconn *h)
{
    char line[ADC_LINE_MAX];
    char sid_text[ADC_SID_CHARS + 1];

    if (h->state != HUB_NORMAL)
        return -1;

    adc_sid_encode(h->sid, sid_text);

    /* An update carries only what changed -- and emphatically not PD. The
       hub keeps the rest; INF is a partial update in both directions. */
    {
        char ss_text[24];

        /* 64-bit: a share over 4GB printed through (unsigned long) on DOS
           would tell the hub the remainder modulo 4GB. */
        adc_u64_dec(share_bytes(h), ss_text, sizeof ss_text);
        snprintf(line, sizeof line, "BINF %s SS%s SF%u",
                 sid_text, ss_text, share_files(h));
    }

    return send_line(h, line);
}

int hub_update_desc(hubconn *h, const char *desc)
{
    char line[ADC_LINE_MAX];
    char sid_text[ADC_SID_CHARS + 1];
    char esc[128];

    if (h->state != HUB_NORMAL)
        return -1;

    copy_field(h->desc, sizeof h->desc, desc ? desc : "");
    if (adc_escape(esc, sizeof esc, h->desc) < 0)
        return -1;

    adc_sid_encode(h->sid, sid_text);

    /* A partial INF, like the share and slot updates: only DE. An empty
       description is still sent -- clearing one has to be possible. */
    snprintf(line, sizeof line, "BINF %s DE%s", sid_text, esc);
    return send_line(h, line);
}

int hub_update_slots(hubconn *h, unsigned int free_slots)
{
    char line[ADC_LINE_MAX];
    char sid_text[ADC_SID_CHARS + 1];

    if (h->state != HUB_NORMAL)
        return -1;

    adc_sid_encode(h->sid, sid_text);
    snprintf(line, sizeof line, "BINF %s SL%u", sid_text, free_slots);
    return send_line(h, line);
}

/* ---------------------------------------------------------- dispatching */

static void handle_sup(hubconn *h, const adc_msg *m)
{
    int i;
    int tiger_ok = 0;

    for (i = 0; i < m->nparam; i++) {
        const char *p = m->param[i];
        if (strcmp(p, "ADTIGR") == 0)
            tiger_ok = 1;
        /* A hub that REMOVES tiger support mid-session would leave us unable
           to answer a password challenge, so note it rather than ignore. */
        if (strcmp(p, "RMTIGR") == 0)
            tiger_ok = 0;
    }

    if (!tiger_ok && h->state == HUB_PROTOCOL) {
        /* Not fatal by itself -- plenty of hubs omit it and still work --
           but worth recording if the login later fails. */
        snprintf(h->err, sizeof h->err,
                 "hub did not advertise ADTIGR; password auth may fail");
    }
}

static void handle_status(hubconn *h, const adc_msg *m)
{
    int severity = 0, code = 0;
    const char *desc = "";

    if (m->nparam >= 1) {
        /* The code is three digits: first is severity, last two the error. */
        const char *s = m->param[0];
        if (s[0] >= '0' && s[0] <= '9') {
            severity = s[0] - '0';
            code = (int)strtol(s + 1, NULL, 10);
        }
    }
    if (m->nparam >= 2)
        desc = m->param[1];

    if (h->cb.on_status)
        h->cb.on_status(h->cb.user, severity, code, desc);

    if (severity == 2) {
        /* Fatal. Spell out the login failures, because a bare code is
           useless to whoever is looking at an 80x25 screen. */
        const char *why;

        switch (code) {
        case 11: why = "hub is full"; break;
        case 21: why = "nick is invalid"; break;
        case 22: why = "nick is already taken"; break;
        case 23: why = "wrong password"; break;
        case 24: why = "CID already in use on this hub"; break;
        case 25: why = "nick and password do not match a registered user"; break;
        case 26: why = "registered users only"; break;
        case 27: why = "hub rejected our PID (CID derivation is wrong?)"; break;
        case 30: why = "banned"; break;
        case 31: why = "permanently banned"; break;
        case 43: why = "a required INF field is missing or invalid"; break;
        case 44: why = "command sent in the wrong state"; break;
        case 47: why = "no common hash algorithm with the hub"; break;
        default: why = NULL; break;
        }

        if (why && desc[0])
            snprintf(h->err, sizeof h->err, "%s (hub says: %s)", why, desc);
        else if (why)
            snprintf(h->err, sizeof h->err, "%s", why);
        else if (desc[0])
            snprintf(h->err, sizeof h->err, "hub error %d: %s", code, desc);
        else
            snprintf(h->err, sizeof h->err, "hub error %d", code);

        goto_state(h, HUB_FAILED);
    }
}

static int handle_inf(hubconn *h, const adc_msg *m)
{
    adc_sid sid;
    adc_user *u;
    int created = 0;

    if (m->nparam < 1 || adc_sid_decode(m->param[0], &sid) != 0)
        return 0;                       /* malformed; ignore rather than die */

    u = adc_userlist_get(&h->users, sid, &created);
    if (!u) {
        set_err(h, "out of memory tracking users", NULL, -1);
        return -1;
    }

    merge_inf(u, m);

    if (sid == h->sid) {
        /* The hub echoing our own INF is the acknowledgement that login
           succeeded -- a definite signal, rather than inferring it from a
           pause in traffic. */
        h->own_inf_seen = 1;
        if (h->state == HUB_IDENTIFY || h->state == HUB_VERIFY)
            goto_state(h, HUB_NORMAL);
    }

    if (created) {
        if (h->cb.on_user_join)
            h->cb.on_user_join(h->cb.user, u);
    } else {
        if (h->cb.on_user_update)
            h->cb.on_user_update(h->cb.user, u);
    }
    return 0;
}

static void handle_quit(hubconn *h, const adc_msg *m)
{
    adc_sid sid;
    adc_user *u;
    const char *reason;
    char nick[ADC_NICK_MAX];

    if (m->nparam < 1 || adc_sid_decode(m->param[0], &sid) != 0)
        return;

    u = adc_userlist_find(&h->users, sid);
    nick[0] = '\0';
    if (u)
        copy_field(nick, sizeof nick, u->nick);

    reason = adc_field(m, "MS");

    /* IQUI for our own SID means the hub is disconnecting us. */
    if (sid == h->sid) {
        if (reason && reason[0])
            snprintf(h->err, sizeof h->err, "disconnected by hub: %s", reason);
        else
            snprintf(h->err, sizeof h->err, "disconnected by hub");
        goto_state(h, HUB_FAILED);
    }

    adc_userlist_remove(&h->users, sid);

    if (h->cb.on_user_part)
        h->cb.on_user_part(h->cb.user, sid, nick, reason ? reason : "");
}

static void handle_msg(hubconn *h, const adc_msg *m)
{
    adc_sid from;
    const adc_user *u = NULL;
    const char *text;
    int hdr = adc_hdr_params(m->type);

    /* IMSG is from the hub itself and has no SID. */
    if (m->type == 'I') {
        if (m->nparam >= 1 && h->cb.on_chat)
            h->cb.on_chat(h->cb.user, NULL, m->param[0], 0);
        return;
    }

    if (m->nparam < hdr + 1)
        return;
    if (adc_sid_decode(m->param[0], &from) != 0)
        return;

    text = m->param[hdr];
    u = adc_userlist_find(&h->users, from);

    /* A PM is a D/E message carrying PM<sid>; without it, a direct message
       is still public chat addressed to one person. */
    if ((m->type == 'D' || m->type == 'E') && adc_field(m, "PM") != NULL) {
        if (h->cb.on_private)
            h->cb.on_private(h->cb.user, u, text);
        return;
    }

    if (h->cb.on_chat)
        h->cb.on_chat(h->cb.user, u, text, from == h->sid);
}

static int handle_gpa(hubconn *h, const adc_msg *m)
{
    char response[ADC_CID_CHARS + 1];
    char line[ADC_LINE_MAX];

    if (m->nparam < 1) {
        set_err(h, "hub sent GPA with no challenge", NULL, -1);
        goto_state(h, HUB_FAILED);
        return -1;
    }

    if (h->password[0] == '\0') {
        snprintf(h->err, sizeof h->err,
                 "hub wants a password but none is configured");
        goto_state(h, HUB_FAILED);
        return -1;
    }

    if (adc_password_response(h->password, m->param[0],
                              response, sizeof response) != 0) {
        set_err(h, "could not compute the password response "
                   "(bad base32 challenge?)", NULL, -1);
        goto_state(h, HUB_FAILED);
        return -1;
    }

    snprintf(line, sizeof line, "HPAS %s", response);
    goto_state(h, HUB_VERIFY);
    return send_line(h, line);
}

static int dispatch(hubconn *h, const adc_msg *m)
{
    const char *cmd = m->cmd;

    if (m->type == 0)
        return 0;                       /* keepalive */

    if (strcmp(cmd, "SUP") == 0) {
        handle_sup(h, m);
        return 0;
    }

    if (strcmp(cmd, "SID") == 0) {
        if (m->nparam < 1 || adc_sid_decode(m->param[0], &h->sid) != 0) {
            set_err(h, "hub sent an unusable SID", NULL, -1);
            goto_state(h, HUB_FAILED);
            return -1;
        }
        /* We may now identify ourselves. */
        goto_state(h, HUB_IDENTIFY);
        return send_own_inf(h);
    }

    if (strcmp(cmd, "INF") == 0) {
        if (m->type == 'I') {
            const char *v = adc_field(m, "NI");
            if (v)
                copy_field(h->hubname, sizeof h->hubname, v);
            return 0;
        }
        return handle_inf(h, m);
    }

    if (strcmp(cmd, "GPA") == 0)
        return handle_gpa(h, m);

    if (strcmp(cmd, "STA") == 0) {
        handle_status(h, m);
        return h->state == HUB_FAILED ? -1 : 0;
    }

    if (strcmp(cmd, "QUI") == 0) {
        handle_quit(h, m);
        return h->state == HUB_FAILED ? -1 : 0;
    }

    if (strcmp(cmd, "MSG") == 0) {
        handle_msg(h, m);
        return 0;
    }

    if (strcmp(cmd, "CTM") == 0) {
        /* "Connect to me": they are active and want us to dial them.
           DCTM <from> <to> <protocol> <port> <token> */
        adc_sid from;
        adc_user *u;
        const char *proto;
        int port;

        if (m->nparam < 5)
            return 0;
        if (adc_sid_decode(m->param[0], &from) != 0)
            return 0;

        proto = m->param[2];
        if (strcmp(proto, "ADC/1.0") != 0) {
            /* We do not do ADCS client-to-client, and saying so is better
               than connecting and failing the handshake. */
            char line[ADC_LINE_MAX];
            char me[ADC_SID_CHARS + 1], them[ADC_SID_CHARS + 1];

            adc_sid_encode(h->sid, me);
            adc_sid_encode(from, them);
            snprintf(line, sizeof line,
                     "DSTA %s %s 141 Protocol\\sunsupported PR%s",
                     me, them, proto);
            send_line(h, line);
            return 0;
        }

        port = (int)strtol(m->param[3], NULL, 10);
        u = adc_userlist_find(&h->users, from);

        if (h->on_ctm && port > 0) {
            /* The address comes from the user list, not from the message:
               a peer does not get to tell us to dial an arbitrary host. */
            const char *host = (u && u->ip[0]) ? u->ip : NULL;

            if (host)
                h->on_ctm(h->cb.user, host, port, m->param[4]);
            else
                snprintf(h->err, sizeof h->err,
                         "a peer asked us to connect but the hub never "
                         "told us their address");
        }
        return 0;
    }

    if (strcmp(cmd, "RCM") == 0) {
        /* "Reverse connect to me": they are passive and want OUR address.
           We can only help if we are active. */
        adc_sid from;

        if (m->nparam < 3)
            return 0;
        if (adc_sid_decode(m->param[0], &from) != 0)
            return 0;

        if (!h->active)
            return 0;               /* both passive: nothing to be done */

        return hub_request_connection(h, from, m->param[m->nparam - 1]);
    }

    if (strcmp(cmd, "SCH") == 0) {
        /* A search from someone else. Answering it is the entire point of
           having a share: on a real hub each client replies for itself. */
        share_file *hits[SHARE_MAX_REPLIES_ACTIVE];
        adc_sid from = ADC_SID_NONE;
        const char *token;
        int n, i;

        if (!h->share || h->state != HUB_NORMAL)
            return 0;

        if (m->nparam < 1 || adc_sid_decode(m->param[0], &from) != 0)
            return 0;

        /* Our own search comes back to us as a broadcast; answering it
           would be absurd and would pollute our own result list. */
        if (from == h->sid)
            return 0;

        /* Passive clients get fewer results by convention, and we are
           passive until Phase 6 makes us reachable. */
        n = share_match(h->share, m, hits, SHARE_MAX_REPLIES_PASSIVE);
        token = adc_field(m, "TO");

        for (i = 0; i < n; i++) {
            char line[ADC_LINE_MAX];
            char path_esc[SCAN_ESC_MAX];
            char b32[BASE32_TIGER_LEN + 1];
            char me[ADC_SID_CHARS + 1];
            char them[ADC_SID_CHARS + 1];

            if (adc_escape(path_esc, sizeof path_esc,
                           share_virt(h->share, hits[i])) < 0)
                continue;

            base32_encode(b32, sizeof b32, hits[i]->tth, TTH_HASH_LEN);
            adc_sid_encode(h->sid, me);
            adc_sid_encode(from, them);

            /* DRES goes direct to the asker, not broadcast: a result is of
               no interest to anyone else on the hub. SL is our free slot
               count, which is what tells them whether to bother. */
            snprintf(line, sizeof line,
                     "DRES %s %s FN%s SI%lu SL%u TR%s",
                     me, them, path_esc,
                     (unsigned long)hits[i]->size, h->slots, b32);

            if (token && *token) {
                char tok_esc[64];
                if (adc_escape(tok_esc, sizeof tok_esc, token) >= 0) {
                    size_t have = strlen(line);
                    snprintf(line + have, sizeof line - have, " TO%s",
                             tok_esc);
                }
            }

            if (send_line(h, line) != 0)
                break;
            h->replies_sent++;
        }

        return 0;
    }

    if (strcmp(cmd, "RES") == 0) {
        if (search_add_result(&h->search, m) && h->cb.on_result)
            h->cb.on_result(h->cb.user,
                            &h->search.results[h->search.count - 1]);
        return 0;
    }

    /* Everything else (SCH from other users, CTM, ...) belongs to later
       phases. Silence is correct: an unknown command is not an error. */
    return 0;
}

int hub_search(hubconn *h, const search_query *q, unsigned long now)
{
    char line[ADC_LINE_MAX];

    if (h->state != HUB_NORMAL) {
        set_err(h, "not logged in yet", NULL, -1);
        return -1;
    }

    if (search_build(&h->search, h->sid, q, line, sizeof line, now) != 0) {
        set_err(h, "nothing to search for", NULL, -1);
        return -1;
    }

    return send_line(h, line);
}

/* -------------------------------------------------------------- public */

int hub_init(hubconn *h, transport *t, const char *nick, const char *password,
             const adc_identity *id, const hub_callbacks *cb)
{
    memset(h, 0, sizeof *h);

    h->t = t;
    h->state = HUB_PROTOCOL;
    h->slots = 1;
    h->id = *id;
    h->keepalive_secs = HUB_KEEPALIVE_SECS;
    h->dead_secs = HUB_DEAD_SECS;

    copy_field(h->nick, sizeof h->nick, nick);
    if (password)
        copy_field(h->password, sizeof h->password, password);
    if (cb)
        h->cb = *cb;

    linebuf_init(&h->in);
    adc_userlist_init(&h->users);
    search_init(&h->search);

    if (h->nick[0] == '\0') {
        set_err(h, "no nick configured", NULL, -1);
        h->state = HUB_FAILED;
        return -1;
    }

    return send_line(h, "HSUP " SUP_FEATURES);
}

void hub_free(hubconn *h)
{
    adc_userlist_free(&h->users);
    /* The password is the one secret in this struct; do not leave it in a
       freed-but-not-cleared buffer. */
    memset(h->password, 0, sizeof h->password);
}

int hub_feed(hubconn *h, const char *data, size_t len)
{
    char line[ADC_LINE_MAX];

    /* Any byte from the hub counts as life, including a keepalive newline
       that parses to nothing. */
    if (len > 0)
        h->last_rx = h->now;

    if (linebuf_feed(&h->in, data, len) != 0) {
        /* Over-long line: the framer resynchronises at the next newline, so
           report it but keep the session. */
        if (h->cb.on_status)
            h->cb.on_status(h->cb.user, 1, 40, "over-long message dropped");
    }

    while (linebuf_next(&h->in, line)) {
        adc_msg m;

        if (adc_parse(&m, line) != 0) {
            if (h->cb.on_status)
                h->cb.on_status(h->cb.user, 1, 40, "unparseable message");
            continue;
        }

        if (dispatch(h, &m) != 0)
            return -1;
    }
    return 0;
}

int hub_poll(hubconn *h)
{
    char buf[1024];
    int progress = 0;
    int guard;

    /* Bounded so a fast hub cannot starve the rest of the main loop -- the
       UI still has to redraw during a 5,000-user login flood. */
    for (guard = 0; guard < 32; guard++) {
        int n = tp_read(h->t, buf, sizeof buf);

        if (n == TP_AGAIN)
            break;
        if (n == TP_CLOSED) {
            if (h->state != HUB_FAILED && h->err[0] == '\0')
                snprintf(h->err, sizeof h->err, "hub closed the connection");
            goto_state(h, HUB_FAILED);
            return -1;
        }
        if (n < 0) {
            if (h->err[0] == '\0')
                snprintf(h->err, sizeof h->err, "read error");
            goto_state(h, HUB_FAILED);
            return -1;
        }

        progress = 1;
        if (hub_feed(h, buf, (size_t)n) != 0)
            return -1;

        if ((size_t)n < sizeof buf)
            break;              /* drained what was available */
    }

    return progress;
}

int hub_tick(hubconn *h, unsigned long now_secs)
{
    h->now = now_secs;

    if (h->state == HUB_FAILED)
        return -1;

    /* First tick establishes the baseline. Without this, timestamps left at
       zero would look like "silent since the epoch" and fail immediately. */
    if (h->last_rx == 0) {
        h->last_rx = now_secs;
        if (h->last_tx == 0)
            h->last_tx = now_secs;
        return 0;
    }

    /* Only police liveness once logged in. During login the hub may
       legitimately take its time, and main() already has its own timeout
       for a hub that never finishes the handshake. */
    if (h->state != HUB_NORMAL)
        return 0;

    if (h->dead_secs && now_secs - h->last_rx >= h->dead_secs) {
        snprintf(h->err, sizeof h->err,
                 "hub stopped responding (nothing for %lu seconds)",
                 now_secs - h->last_rx);
        goto_state(h, HUB_FAILED);
        return -1;
    }

    if (h->keepalive_secs && now_secs - h->last_tx >= h->keepalive_secs) {
        /* An empty line: a message with no type or command, which ADC
           defines as a no-op. Cheaper than any real command and carries no
           side effects. */
        if (send_line(h, "") != 0) {
            goto_state(h, HUB_FAILED);
            return -1;
        }
    }

    return 0;
}

int hub_say(hubconn *h, const char *text)
{
    char esc[ADC_LINE_MAX];
    char line[ADC_LINE_MAX];
    char sid_text[ADC_SID_CHARS + 1];

    if (h->state != HUB_NORMAL) {
        set_err(h, "not logged in yet", NULL, -1);
        return -1;
    }
    if (adc_escape(esc, sizeof esc / 2, text) < 0) {
        set_err(h, "message too long", NULL, -1);
        return -1;
    }

    adc_sid_encode(h->sid, sid_text);
    snprintf(line, sizeof line, "BMSG %s %s", sid_text, esc);
    return send_line(h, line);
}

int hub_pm(hubconn *h, adc_sid to, const char *text)
{
    char esc[ADC_LINE_MAX];
    char line[ADC_LINE_MAX];
    char from_text[ADC_SID_CHARS + 1];
    char to_text[ADC_SID_CHARS + 1];

    if (h->state != HUB_NORMAL) {
        set_err(h, "not logged in yet", NULL, -1);
        return -1;
    }
    if (adc_escape(esc, sizeof esc / 2, text) < 0) {
        set_err(h, "message too long", NULL, -1);
        return -1;
    }

    adc_sid_encode(h->sid, from_text);
    adc_sid_encode(to, to_text);

    /* PM<our sid> marks this as private and tells the recipient which
       conversation it belongs to. */
    snprintf(line, sizeof line, "DMSG %s %s %s PM%s",
             from_text, to_text, esc, from_text);
    return send_line(h, line);
}

int hub_raw(hubconn *h, const char *line)
{
    return send_line(h, line);
}

int hub_quit(hubconn *h)
{
    /* Best effort: if the socket is already gone there is nothing to say. */
    return send_line(h, "HDSC");
}

const char *hub_error(const hubconn *h)
{
    return h->err[0] ? h->err : "no error";
}

const char *hub_state_name(hub_state s)
{
    switch (s) {
    case HUB_PROTOCOL: return "PROTOCOL";
    case HUB_IDENTIFY: return "IDENTIFY";
    case HUB_VERIFY:   return "VERIFY";
    case HUB_NORMAL:   return "NORMAL";
    case HUB_FAILED:   return "FAILED";
    default:           return "?";
    }
}

const adc_user *hub_self(hubconn *h)
{
    if (!h->own_inf_seen)
        return NULL;
    return adc_userlist_find(&h->users, h->sid);
}
