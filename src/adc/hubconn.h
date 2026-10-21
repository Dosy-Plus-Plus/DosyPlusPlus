/* hubconn.h - the hub connection state machine.
 *
 *   PROTOCOL --ISUP--> IDENTIFY --ISID,BINF--> NORMAL
 *                          |
 *                       IGPA v
 *                        VERIFY --HPAS,BINF--> NORMAL
 *
 * Non-blocking throughout: hub_poll() reads whatever is available, frames
 * whole lines, and dispatches. A half-received message waits in the line
 * buffer. Nothing here calls recv() -- all I/O goes through the transport
 * vtable, so a TLS transport can be dropped in later.
 *
 * Events go out through callbacks rather than being printed, so the Phase 3
 * UI can consume the same state machine the Phase 2 console client does.
 *
 * No DOS dependency; unit-tested natively against a memory transport.
 */
#ifndef DOSYPP_HUBCONN_H
#define DOSYPP_HUBCONN_H

#include "../net/transport.h"
#include "ident.h"
#include "tokenize.h"
#include "userlist.h"
#include "search.h"
#include "../share/index.h"
#include "../share/scan.h"   /* SCAN_ESC_MAX */

typedef enum {
    HUB_PROTOCOL,     /* HSUP sent, waiting for ISUP/ISID */
    HUB_IDENTIFY,     /* BINF sent, waiting for acceptance or IGPA */
    HUB_VERIFY,       /* HPAS sent, waiting for acceptance */
    HUB_NORMAL,       /* logged in */
    HUB_FAILED        /* gave up; see hub_error() */
} hub_state;

#define HUB_NAME_MAX 64
#define HUB_ERR_MAX  192

typedef struct hubconn hubconn;

/* A peer asked US to connect to them: CTM arrived. The callback is how the
   application learns where to dial. */
typedef void (*hub_ctm_fn)(void *user, const char *host, int port,
                           const char *token);

/* All optional. `user` is passed back as the first argument. */
typedef struct {
    void *user;

    void (*on_state)(void *user, hub_state state);
    /* Public chat. `from` may be NULL for a hub message (IMSG). `me` is
       non-zero when the message is our own, echoed back. */
    void (*on_chat)(void *user, const adc_user *from, const char *text,
                    int me);
    void (*on_private)(void *user, const adc_user *from, const char *text);
    /* A search result arrived and was accepted into the table. */
    void (*on_result)(void *user, const search_result *r);
    /* A user appeared, changed, or left. */
    void (*on_user_join)(void *user, const adc_user *u);
    void (*on_user_update)(void *user, const adc_user *u);
    void (*on_user_part)(void *user, adc_sid sid, const char *nick,
                         const char *reason);
    /* ISTA. severity 0 info, 1 recoverable, 2 fatal. */
    void (*on_status)(void *user, int severity, int code, const char *desc);
} hub_callbacks;

struct hubconn {
    transport      *t;
    hub_state       state;
    linebuf         in;

    adc_identity    id;
    adc_sid         sid;                   /* ours, from ISID */
    char            nick[ADC_NICK_MAX];
    char            password[64];
    char            desc[ADC_DESC_MAX];
    unsigned int    slots;

    char            hubname[HUB_NAME_MAX];
    char            err[HUB_ERR_MAX];

    adc_userlist    users;
    search_state    search;

    /* The share we answer searches from. Borrowed, not owned: the index
       belongs to the application, which also scans and hashes it. NULL
       means we answer nothing, which is correct before Phase 5 has run. */
    share_index    *share;
    unsigned long   replies_sent;   /* for the share door */

    /* Active mode. A passive client cannot be dialled, so two passive
       clients can never transfer to each other -- which is why this is
       worth configuring rather than assuming. */
    hub_ctm_fn      on_ctm;
    int             active;
    int             listen_port;
    char            local_ip[20];   /* what we tell peers to dial */
    hub_callbacks   cb;

    /* Set once the hub has told us about ourselves, which is how we know
       the login flood is over without guessing from timing. */
    int             own_inf_seen;

    /* Liveness. Time is passed in rather than read here so tests can drive
       it without sleeping. */
    unsigned long   now;
    unsigned long   last_tx;
    unsigned long   last_rx;
    unsigned int    keepalive_secs;   /* 0 disables sending keepalives */
    unsigned int    dead_secs;        /* 0 disables the dead-hub check */
};

/* Hubs disconnect clients that say nothing. An empty line is a no-op
   message in ADC, which is exactly what it is for. 60s is comfortably
   inside the shortest timeouts seen in the wild (typically 120-180s). */
#define HUB_KEEPALIVE_SECS 60

/* If nothing arrives for this long the connection is dead even though the
   socket still looks open -- a NAT or a modem can drop the path silently,
   and on DOS there is no TCP keepalive to notice for us. Must be a healthy
   multiple of the hub's own keepalive so a quiet hub is not mistaken for a
   dead one. */
#define HUB_DEAD_SECS 300

/* Set up over an already-connected transport and send HSUP.
   nick is required; password may be NULL or empty. */
int hub_init(hubconn *h, transport *t, const char *nick, const char *password,
             const adc_identity *id, const hub_callbacks *cb);

void hub_free(hubconn *h);

/* Read and dispatch whatever is available. Returns:
     1  progress made
     0  nothing to do right now
    -1  connection lost or protocol failure (see hub_error) */
int hub_poll(hubconn *h);

/* Feed bytes directly, bypassing the transport. For tests and for replaying
   captured sessions. Returns 0, or -1 on protocol failure. */
int hub_feed(hubconn *h, const char *data, size_t len);

/* Call once per main-loop iteration with the current time in seconds.
   Sends a keepalive when due and fails the connection if the hub has gone
   silent. Returns 0, or -1 if the connection is now dead. */
int hub_tick(hubconn *h, unsigned long now_secs);

/* Start a search. Returns 0, or -1 (see hub_error) if the query is empty
   or we are not logged in. Results arrive via on_result and accumulate in
   h->search. */
int hub_search(hubconn *h, const search_query *q, unsigned long now);

/* Attach a share. Searches from other users are then answered from it, and
   our INF reports its size. Pass NULL to stop answering. */
void hub_set_share(hubconn *h, share_index *ix);

/* Re-send our INF with the current share totals. Hubs with a minimum share
   only let you in once this reports enough, so it must be sent again when
   hashing finishes rather than only at login. */
int hub_update_share(hubconn *h);

/* Tell the hub how many upload slots are free. Other clients read SL to
   decide whether asking us for a file is worth it, so it is worth keeping
   current rather than advertising a constant. Sent only when the number
   actually changes -- an INF per byte transferred would be flooding. */
int hub_update_slots(hubconn *h, unsigned int free_slots);

/* Push a new description (the DE field). Returns 0, or -1 if we are not
   logged in. The hub may ignore it; there is no reply either way, which is
   how INF works. */
int hub_update_desc(hubconn *h, const char *desc);

/* Declare ourselves reachable. Must be set before login for the INF to
   advertise it; `ip` may be "0.0.0.0" to let the hub substitute what it
   sees, which is what you want behind NAT. */
void hub_set_active(hubconn *h, int active, int port, const char *ip);

/* Ask `to` for a connection about `token`.
 *
 * Sends CTM when we are active ("dial me at this port") and RCM when we are
 * not ("dial me is impossible; send me a CTM"). Returns 0, or -1 if we are
 * passive AND the peer is passive, in which case no connection is possible
 * and the caller should try another source. */
int hub_request_connection(hubconn *h, adc_sid to, const char *token);

void hub_set_ctm_handler(hubconn *h, hub_ctm_fn fn);

int hub_say(hubconn *h, const char *text);
int hub_pm(hubconn *h, adc_sid to, const char *text);
/* Send a raw line (the /raw command). A newline is appended. */
int hub_raw(hubconn *h, const char *line);
int hub_quit(hubconn *h);

const char *hub_error(const hubconn *h);
const char *hub_state_name(hub_state s);

/* Our own record, once the hub has echoed it. NULL before then. */
const adc_user *hub_self(hubconn *h);

#endif /* DOSYPP_HUBCONN_H */
