/* hublist.h - where to find hubs.
 *
 * A public hub list is an XML file on the web, usually bzip2-compressed,
 * listing addresses with a name, a description and how many people are on
 * each. Fetching one is the difference between a client you can use and a
 * client you can use if somebody tells you an address.
 *
 * It is a list of claims, not facts: the user counts are whatever each hub
 * reported to whoever built the list, and the descriptions are written by
 * hub owners. Nothing here is trusted beyond being bounded and
 * transliterated for display.
 *
 * Bounded at 400 entries. A full public list has thousands, and this is a
 * machine with 16MB whose screen shows eighteen rows: the entries kept are
 * the ones with the most users, which is the order anyone would sort by
 * anyway. The door says when the list was cut short.
 */
#ifndef DOSYPP_HUBLIST_H
#define DOSYPP_HUBLIST_H

#include <stddef.h>

#define HUBLIST_MAX       400
#define HUBLIST_ADDR_MAX  64
#define HUBLIST_NAME_MAX  40
#define HUBLIST_DESC_MAX  64

/* Where a fetched list is kept while it is read. 8.3, like everything on a
   DOS disk, and removed once parsed. */
#define HUBLIST_FILE "HUBLIST.TMP"

/* A sensible default: xml.bz2, served over plain HTTP. Changed in the
   settings, because lists come and go. */
#define HUBLIST_DEFAULT_URL "http://www.te-home.net/?do=hublist&get=hublist.xml.bz2"

typedef struct {
    char          addr[HUBLIST_ADDR_MAX];   /* adc://host:port */
    char          name[HUBLIST_NAME_MAX];
    char          desc[HUBLIST_DESC_MAX];
    unsigned long users;
    unsigned long shared_gb;                /* GB, because bytes overflow */
} hublist_entry;

typedef struct {
    hublist_entry entries[HUBLIST_MAX];
    int           count;
    int           truncated;    /* the list was longer than we keep */
    int           skipped;      /* entries that were not ADC hubs */
    char          err[160];
} hublist;

void hublist_init(hublist *hl);

/* Parse a hub list from a file, compressed or not: a .bz2 and plain XML
   are told apart by their first bytes rather than by the file name, since
   the name comes from a URL and lies often enough. Returns the number of
   entries kept, or -1 with the reason in hl->err. */
int hublist_load(hublist *hl, const char *path);

/* Fetch a list and parse it. Returns the number of entries, or -1. */
int hublist_fetch(hublist *hl, const char *url,
                  int (*progress)(void *user, unsigned long got,
                                  unsigned long total),
                  void *user);

/* Sort by user count, descending. The order a person wants. */
void hublist_sort(hublist *hl);

#endif /* DOSYPP_HUBLIST_H */
