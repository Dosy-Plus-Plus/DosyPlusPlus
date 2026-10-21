/* userlist.h - who is on the hub.
 *
 * Built entirely from BINF messages: the hub sends one per existing user at
 * login, then one whenever anything changes, and IQUI when someone leaves.
 * An INF is a *partial update* -- a user changing their description sends
 * only DE -- so records are merged, never replaced. Treating INF as a full
 * record is a real bug that shows up as nicks vanishing mid-session.
 *
 * Sized for a busy hub: the plan's target is 5,000 users without trouble,
 * which at sizeof(adc_user) is well under a megabyte. Lookup is by SID
 * through a small open-addressed index, because a linear scan per message
 * on a 5,000-user hub during the login flood is noticeable on a 486.
 *
 * No DOS dependency; unit-tested natively.
 */
#ifndef DOSYPP_USERLIST_H
#define DOSYPP_USERLIST_H

#include "ident.h"

#define ADC_NICK_MAX 32
#define ADC_TAG_MAX  24
#define ADC_DESC_MAX 32

/* CT bits from the spec. */
#define ADC_CT_BOT        1
#define ADC_CT_REGISTERED 2
#define ADC_CT_OP         4
#define ADC_CT_SUPER      8
#define ADC_CT_OWNER      16
#define ADC_CT_HUB        32

typedef struct {
    adc_sid       sid;
    char          nick[ADC_NICK_MAX];
    char          tag[ADC_TAG_MAX];        /* VE, or AP + VE joined */
    char          desc[ADC_DESC_MAX];      /* DE */
    char          cid[ADC_CID_CHARS + 1];  /* ID */
    tiger_u64     share;                   /* SS, bytes */
    unsigned long files;                   /* SF */
    unsigned int  slots;                   /* SL */
    unsigned int  hubs_normal;             /* HN */
    unsigned int  hubs_reg;                /* HR */
    unsigned int  hubs_op;                 /* HO */
    unsigned int  ct;                      /* CT bitmask */
    char          ip[20];                  /* I4, as the hub reports it */
    char          su[24];                  /* SU verbatim, for diagnostics */
    int           passive;                 /* no TCP4 in SU. Starts at 1:
                                              reachability must be claimed,
                                              never assumed. */
    int           in_use;
} adc_user;

/* Dense record array plus an open-addressed SID index.
 *
 * The index earns its keep at login: the hub sends one BINF per existing
 * user, so joining a 5,000-user hub is 5,000 lookups in a row. A linear scan
 * would make that 12.5 million comparisons -- fine on a desktop, visible on
 * a 486. Deletions use tombstones so IQUI does not force a rehash. */
typedef struct {
    adc_user     *users;
    unsigned int  count;        /* live records */
    unsigned int  cap;          /* slots in users[] */

    unsigned long *idx;         /* 0 = free, TOMBSTONE, else slot+1 */
    unsigned int   idx_cap;     /* power of two */
    unsigned int   idx_used;    /* live + tombstoned entries */
} adc_userlist;

void adc_userlist_init(adc_userlist *ul);
void adc_userlist_free(adc_userlist *ul);

/* Find by SID, or NULL. */
adc_user *adc_userlist_find(adc_userlist *ul, adc_sid sid);

/* Find by nick, case-insensitively (the hub enforces uniqueness that way).
   Used by /pm <nick>. */
adc_user *adc_userlist_find_nick(adc_userlist *ul, const char *nick);

/* Get the record for a SID, creating it if new. Returns NULL only if memory
   ran out. *created is set to 1 for a new record, 0 for an existing one. */
adc_user *adc_userlist_get(adc_userlist *ul, adc_sid sid, int *created);

/* Remove a user (IQUI). Returns 1 if someone was removed. */
int adc_userlist_remove(adc_userlist *ul, adc_sid sid);

void adc_userlist_clear(adc_userlist *ul);

/* Iteration: index 0..cap-1, skipping holes. Returns NULL past the end. */
adc_user *adc_userlist_at(adc_userlist *ul, unsigned int index);

/* Total shared bytes across the hub, for the status bar. */
tiger_u64 adc_userlist_total_share(adc_userlist *ul);

#endif /* DOSYPP_USERLIST_H */
