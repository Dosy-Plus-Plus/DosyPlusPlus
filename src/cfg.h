/* cfg.h - DOSYPP.INI.
 *
 * Plain `key = value` lines, `;` or `#` comments, sections ignored for now.
 * 8.3 filename, written back in place.
 *
 * The password is stored in the clear. There is nowhere better on a DOS
 * machine -- no keychain, no file permissions -- and obfuscating it would
 * only misrepresent how safe it is. The written file says so explicitly.
 *
 * The one field that matters beyond convenience is `pid`: it is this
 * installation's identity. Lose it and you get a new CID, which on a
 * registered hub means the account no longer recognises you. It is generated
 * once, on first run.
 */
#ifndef DOSYPP_CFG_H
#define DOSYPP_CFG_H

#include "adc/ident.h"
#include "share/index.h"

#define CFG_STR_MAX 96

typedef struct {
    char          hub[CFG_STR_MAX];
    int           port;
    char          nick[CFG_STR_MAX];
    char          password[CFG_STR_MAX];
    char          desc[CFG_STR_MAX];
    unsigned int  slots;
    int           sound;        /* PC speaker: on by default, and a
                                   legitimate thing to turn off */

    /* Where to fetch a public hub list from. Empty means the built-in
       default, which is a list that exists today and may not tomorrow --
       hence a setting. */
    char          hublist[160];

    /* Music. A BBS played a MOD; this one does too, unless told not to.
       A missing card or a missing file is reported once and then ignored,
       because a client that refuses to run without a sound card would be
       absurd. */
    /* 25 or 50. Fifty is mode 3 with the 8x8 font: twice the chat and
       twice the file list, at the price of a font a CRT renders thinly.
       Anything else is read as 25. */
    int           rows;

    int           music;
    char          music_file[64];
    int           music_rate;
    /* Host builds only: where the mixer's output is written. Empty means
       the backend's own default. A DOS build has a sound card instead and
       ignores it. */
    char          music_wav[64];

    /* Which skin. The whole point of the project is how it looks, so this
       belongs in the file rather than only on the command line. */
    char          theme[16];

    /* Active mode. A passive client cannot be dialled, so two passive
       clients can never transfer to each other -- which is why this is
       worth a setting rather than a guess. Port 0 means "pick one". */
    int           active;
    int           listen_port;

    /* Hash the share automatically after the scan, or wait to be asked.
       On a large share hashing is half an hour of solid disk work, and a
       client that starts doing it unbidden on a machine someone is using
       for something else is not a good guest. Manual mode still scans --
       that is seconds -- and still serves whatever is already in the
       cache. */
    int           hash_auto;

    /* Rescan the share every N minutes, or 0 for never. Off by default:
       a rescan is cheap when nothing changed (the hash cache answers for
       every file whose size and timestamp match) but it is still disk
       work, and a client that starts grinding the drive on its own while
       someone is using the machine for something else is not welcome.
       Anyone who wants it can say so. The floor is one minute. */
    int           rescan_mins;

    /* Directories to share. Repeat the `share =` key once per path; eight
       is plenty for a DOS machine with two drives. */
    char          shares[SHARE_MAX_ROOTS][SHARE_ROOT_MAX];
    int           nshare;
    adc_identity  id;
    int           id_valid;

    /* Set when the file had no usable pid and one was generated, so the
       caller knows it must save. */
    int           dirty;

    /* Problems found while reading the file. Collected rather than printed
       here so the caller decides where they go, but they MUST be shown: a
       share path silently dropped for being one byte too long reads as
       "sharing is broken", and that cost real debugging time. */
    char          warn[4][160];
    int           nwarn;
} cfg;

/* Fill with defaults, then overlay whatever the file provides. A missing
   file is not an error -- first run is normal. Returns 0 always; read
   problems leave defaults in place. */
int cfg_load(cfg *c, const char *path);

/* Returns 0 on success, -1 if the file could not be written. */
int cfg_save(const cfg *c, const char *path);

#endif /* DOSYPP_CFG_H */
