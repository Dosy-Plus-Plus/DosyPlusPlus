/* favs.h - hubs worth coming back to, and the passwords that get you in.
 *
 * The public hub list is for finding hubs; this is for the three or four you
 * actually use. The difference that matters is the password: a registered
 * account is per hub, so one password in DOSYPP.INI is no use to anyone with
 * two accounts. The same goes for the handle -- plenty of people are known
 * differently in different places.
 *
 * Stored in DOSYPP.FAV, as blocks of `key = value` lines:
 *
 *     [hub]
 *     name     = The Attic
 *     address  = adc://attic.example:1511
 *     nick     = Phantom
 *     password = hunter2
 *     auto     = yes
 *
 * Read and written as a whole. It is a handful of entries, not a database,
 * and a format somebody can fix in EDIT when it goes wrong is worth more
 * here than one that is quick to parse.
 *
 * The passwords are in the clear, exactly like the one in DOSYPP.INI, and
 * for the same reason: there is nowhere better on a DOS machine -- no
 * keychain, no file permissions, no user accounts. Obfuscating them would
 * only misrepresent how safe they are. The written file says so at the top,
 * and so does the door.
 */
#ifndef DOSYPP_FAVS_H
#define DOSYPP_FAVS_H

#include <stddef.h>

#define FAVS_FILE      "DOSYPP.FAV"
#define FAVS_MAX       32
#define FAV_NAME_MAX   40
#define FAV_ADDR_MAX   64
#define FAV_NICK_MAX   32
#define FAV_PASS_MAX   64

typedef struct {
    char name[FAV_NAME_MAX];
    char addr[FAV_ADDR_MAX];    /* adc://host:port, or host:port */
    char nick[FAV_NICK_MAX];    /* empty: use the one from DOSYPP.INI */
    char pass[FAV_PASS_MAX];    /* empty: no account, or no password */
    int  autoconnect;           /* dial this one on startup */

    /* Filled in by the UI as a convenience, not stored: how many times a
       connection to this hub has failed this session, so a dead favourite
       stops being retried silently. */
    int  failures;
} fav_entry;

typedef struct {
    fav_entry list[FAVS_MAX];
    int       count;
    int       dirty;            /* changed since it was last written */
    char      err[160];
} favs;

void favs_init(favs *f);

/* Read the file. A missing file is not an error -- nobody has favourites on
   their first run. Returns the number of entries, or -1 if the file exists
   and could not be read at all. A corrupt entry is skipped rather than
   fatal: losing one hub beats losing the file. */
int favs_load(favs *f, const char *path);

/* Write it back. Returns 0, or -1. */
int favs_save(const favs *f, const char *path);

/* Add, or update the entry with the same address. Returns the entry, or
   NULL if the list is full. Marks the list dirty. */
fav_entry *favs_add(favs *f, const char *name, const char *addr);

/* The entry with this address, or NULL. Addresses are compared after
   stripping an `adc://` prefix and a trailing slash, so the same hub added
   from a public list and typed by hand is one entry, not two. */
fav_entry *favs_find(favs *f, const char *addr);

/* Remove by index. Returns 1 if something was removed. */
int favs_remove(favs *f, int index);

/* The one marked for auto-connect, or NULL. The first one wins: two is a
   contradiction, and refusing to start would be a poor way to say so. */
fav_entry *favs_autoconnect(favs *f);

/* Only one entry can be the automatic one, so setting it clears the rest. */
void favs_set_autoconnect(favs *f, int index);

/* Split an entry's address into a host and a port. Returns 0 on success.
   `adc://` is stripped, a missing port means 1511. */
int favs_split(const char *addr, char *host_out, size_t host_sz,
               int *port_out);

#endif /* DOSYPP_FAVS_H */
