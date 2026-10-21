/* transport.h - byte-stream abstraction.
 *
 * Every socket read and write in DOSy++ goes through this vtable. The hub and
 * peer state machines must never call recv()/send() directly: a TLS transport
 * has to be able to slot in beside the plaintext one without touching them.
 *
 * The one rule that is easy to get wrong: a readable socket does NOT imply
 * application data is available. A TLS record can arrive in pieces and yield
 * zero plaintext bytes. Callers poll t_pending() and tolerate TP_AGAIN.
 */
#ifndef DOSYPP_TRANSPORT_H
#define DOSYPP_TRANSPORT_H

#include <stddef.h>

/* Return codes shared by t_read and t_write. Non-negative means byte count. */
#define TP_AGAIN  (-1)  /* no progress right now, try later */
#define TP_CLOSED (-2)  /* orderly shutdown by the peer */
#define TP_ERROR  (-3)  /* connection is dead; close it */

typedef struct transport transport;

struct transport {
    int  (*t_read)(transport *t, void *buf, size_t len);
    int  (*t_write)(transport *t, const void *buf, size_t len);
    /* Bytes of application data known to be buffered and readable without
       touching the network. Plaintext transports return 0. */
    int  (*t_pending)(transport *t);
    void (*t_close)(transport *t);

    int   fd;    /* underlying descriptor, for select(); -1 when closed */
    void *impl;  /* transport-private state */
};

static __inline int tp_read(transport *t, void *b, size_t n)
{
    return t->t_read(t, b, n);
}
static __inline int tp_write(transport *t, const void *b, size_t n)
{
    return t->t_write(t, b, n);
}
static __inline int tp_pending(transport *t)
{
    return t->t_pending(t);
}
static __inline void tp_close(transport *t)
{
    t->t_close(t);
}

/* --- plaintext TCP (tp_plain.c) --- */

/* Connection progress, from tp_plain_connect_start/poll. */
#define TP_CONN_DONE     1
#define TP_CONN_PENDING  0
#define TP_CONN_FAILED  (-1)

/* Begin a non-blocking connect. Returns TP_CONN_DONE if it completed
   immediately (normal for loopback), TP_CONN_PENDING if it is in progress,
   or TP_CONN_FAILED. On failure the transport is left closed.

   Name resolution still blocks: Watt-32's resolver is synchronous and
   writing an async one is not worth it. The resolver timeout is what
   WATTCP.CFG controls. */
int tp_plain_connect_start(transport *t, const char *host, int port);

/* Poll a pending connect. Same three return values. Safe to call after it
   has already completed (returns TP_CONN_DONE). */
int tp_plain_connect_poll(transport *t);

/* Blocking convenience, built on the two above. Waits up to timeout_secs.
   Returns 0 on success, -1 on failure or timeout. */
int tp_plain_connect(transport *t, const char *host, int port,
                     int timeout_secs);

/* --- listening, for active mode ---------------------------------------
 *
 * An active client is reachable, which is what lets two clients connect at
 * all: a passive client cannot accept, so two passive clients can never
 * transfer to each other. Being active is therefore not a nicety.
 *
 * `port` 0 asks the system to choose one; tp_listen_port() reports what was
 * actually bound, which is the number that goes into CTM. */
typedef struct {
    int fd;
    int port;
} tp_listener;

int  tp_listen(tp_listener *l, int port);
void tp_listen_close(tp_listener *l);
int  tp_listen_port(const tp_listener *l);

/* Accept a pending connection into `t`. Returns 1 if one was accepted, 0 if
   none is waiting, -1 on error. Never blocks. */
int  tp_accept(tp_listener *l, transport *t);

const char *tp_plain_lasterr(void);

/* Convenience: write all of buf, spinning over TP_AGAIN. Phase 0 only --
   the event loop replaces this with a proper output queue. */
int tp_write_all(transport *t, const void *buf, size_t len);

#endif /* DOSYPP_TRANSPORT_H */
