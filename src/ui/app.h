/* app.h - the BBS shell: screens, hotkeys, and the redraw loop.
 *
 * Consumes hubconn through its callbacks, exactly as the console client
 * does. The state machine has no idea a UI exists, which is why both can
 * coexist (--console keeps the plain one).
 *
 * Screens are "doors" in BBS terms: full-screen, one at a time, selected by
 * a number key, with a hotkey bar along the bottom.
 */
#ifndef DOSYPP_APP_H
#define DOSYPP_APP_H

#include "../adc/hubconn.h"
#include "../cfg.h"
#include "layout.h"
#include "screen.h"
#include "theme.h"
#include "widget.h"
#include "../snd/pcspkr.h"
#include "../share/index.h"
#include "../share/scan.h"
#include "../share/hashq.h"
#include "../share/namemap.h"
#include "../xfer/queue.h"
#include "../share/browse.h"
#include "../adc/hublist.h"
#include "../adc/favs.h"

typedef enum {
    DOOR_CHAT = 0,     /* 1 - message base */
    DOOR_WHO,          /* 2 - who's online */
    DOOR_FILES,        /* 3 - file areas: search and results */
    DOOR_XFER,         /* 4 - transfers in and out */
    DOOR_SHARE,        /* 5 - your own share: scan and hash progress */
    DOOR_WELCOME,      /* 6 - the hub's MOTD, ANSI and all */
    DOOR_INFO,         /* 7 - node info */
    DOOR_BROWSE,       /* 8 - somebody else's share */
    DOOR_SETUP,        /* 9 - the settings, editable */
    DOOR_HUBS,         /* 0 - the public hub list */
    DOOR_COUNT
} door_id;

/* Which field of the search form has focus. */
typedef enum {
    SF_TERMS = 0,
    SF_EXCLUDE,
    SF_GROUP,
    SF_MINSIZE,
    SF_RESULTS,        /* focus is in the results list */
    SF_COUNT
} search_focus;

/* How much of a hub welcome screen to keep. A full-screen ANSI is about
   4KB; some hubs send several screens. Beyond this it is truncated rather
   than allowed to grow without bound from the network. */
#define MOTD_MAX 8192


typedef struct {
    screen        scr;
    app_layout    lay;
    const theme  *th;
    hubconn      *hub;
    cfg          *conf;

    door_id       door;
    int           running;
    int           need_redraw;

    scrollback    chat;
    inputline     input;
    listbox       who;

    /* File areas. */
    listbox       files;
    search_focus  sfocus;
    inputline     f_terms;
    inputline     f_exclude;
    inputline     f_minsize;
    search_group  f_group;
    search_sort   f_sort;

    /* Our own share. Owned here, borrowed by hubconn. */
    share_index   share;
    /* B was pressed on our own node before the share had been scanned:
       open it as soon as there is something to open. */
    int           browse_self_pending;
    scan_state   *scan;
    hashq        *hq;
    int           share_ready;    /* scanned and fully hashed */

    /* Hashing can be deferred (hash = manual) and paused from the share
       door. Paused means "not spending the disk", not "forgotten": the
       partly-hashed file keeps its position and whatever is already hashed
       stays shared. */
    int           hash_paused;

    /* Seconds actually spent hashing, which is what the rate and the ETA
       are computed from. Wall-clock would include paused time and report a
       throughput this machine never achieved. */
    unsigned long hash_run_secs;
    unsigned long hash_last_tick;
    unsigned long hash_saved;     /* when the cache was last checkpointed */

    /* Where the settings came from, so the setup door writes back to the
       same file rather than guessing at DOSYPP.INI. */
    char          conf_path[128];

    /* The hubs door has two views: the handful you come back to, and the
       public list you find new ones in. They are different enough to be
       two lists and similar enough to be one door. */
    favs          favs;
    listbox       favs_box;
    int           fav_view;         /* 1 = favourites, 0 = public list */
    int           fav_editing;      /* which field, or -1 */
    int           fav_confirm_del;  /* D pressed once on this entry */
    inputline     fav_input;

    /* The public hub list, and the door that shows it. */
    hublist       hubs;
    listbox       hubs_box;
    int           hubs_fetching;
    unsigned long hubs_fetched_at;

    /* A hub the user picked. The app does not own the connection, so it
       asks: main tears the session down and dials this instead. The
       credentials come along because a favourite has its own. */
    char          switch_to[CFG_STR_MAX];
    int           switch_port;
    char          switch_nick[FAV_NICK_MAX];
    char          switch_pass[FAV_PASS_MAX];
    char          switch_label[FAV_NAME_MAX];
    int           want_switch;

    /* The settings door. `editing` is the field being typed into, or -1;
       `dirty` says the file no longer matches what is on screen. */
    listbox       setup;
    int           editing;
    inputline     setup_input;
    int           setup_dirty;

    /* The editor opens with the current value in it, so a long hostname
       can be corrected rather than retyped -- but the first character
       typed replaces it, because replacing is what people usually mean.
       Backspace and the arrows edit instead. */
    int           setup_fresh;

    /* Somebody else's share. One at a time: a DOS machine browsing four
       people simultaneously is not a use case, and each list costs real
       memory. */
    browse_list   browse;
    listbox       browse_box;
    int           browse_ready;
    adc_sid       browse_sid;
    /* Partial browsing. `browse_fetching` is the directory a listing was
       asked for, so the reply can be filed under it; `browse_no_partial`
       remembers a peer that refused one, so the fallback to the whole list
       happens once rather than on every directory. */
    long          browse_fetching;
    int           browse_no_partial;
    /* A directory to walk to as the listings arrive, from --browse-dir.
       Empty when there is nowhere in particular to go. */
    char          browse_goto[128];

    /* Transfers. */
    namemap      *names;

    /* The free-slot count last advertised, so an INF goes out when it
       changes and not otherwise. */
    int           slots_advertised;

    /* Resuming downloads restored from the queue file. A restored request
       knows a hash but no sources -- SIDs do not survive a restart -- so
       the hash is searched for. One at a time, and only until the user
       searches for something themselves, because a search replaces the
       results table they are looking at. */
    unsigned long resume_next_at;
    unsigned long last_user_search;

    /* When the share was last scanned, for the rescan timer. */
    unsigned long scanned_at;
    xfer_queue    xq;
    listbox       xfers;
    tp_listener   listener;
    int           listening;

    /* The hub's welcome text, raw (CP437 but codes intact) so the welcome
       door can render the art. Everything that reaches the chat pane is
       stripped first. */
    char          motd[MOTD_MAX];
    size_t        motd_len;
    int           motd_is_art;
    int           motd_scroll;
    int           motd_rows;

    /* Status line text, and when it should stop being shown. */
    char          status[80];
    unsigned long status_until;

    unsigned long now;          /* seconds, from the caller */
    unsigned long connected_at; /* for the logoff screen's session time */
    unsigned int  nodes_seen;   /* high-water mark of the user list */
    int           connected;
    int           splash_phase; /* drives the greetz scroller */
    unsigned long splash_drawn; /* ms at the last splash redraw */
} app;

/* Set up the UI. Returns 0, or -1 if the display could not be opened
   (reason via vga_lasterr). */
/* `rows` is the display height to lay out for: 25 or 50. Zero means "ask
   the display", which is what the program does -- the flag and the config
   key go to the display layer, not here. */
int app_init(app *a, hubconn *h, cfg *conf, const char *theme_name);



/* Where the config was loaded from. The setup door saves there. */
void app_set_conf_path(app *a, const char *path);

/* Fetch the public hub list now. Exposed for --fetch-hublist, which is how
   the DOS rig drives this without a keyboard. */
void app_fetch_hublist(app *a);

/* Where a hub switch is going, including the credentials for it: a
   favourite carries its own handle and password, because a registered
   account is per hub and one password in DOSYPP.INI is no use to anyone
   with two of them. Empty strings mean "use the ones from the config". */
typedef struct {
    char host[CFG_STR_MAX];
    int  port;
    char nick[FAV_NICK_MAX];
    char pass[FAV_PASS_MAX];
    char label[FAV_NAME_MAX];
} app_hub_target;

/* Has the user picked a different hub? Returns 1 and fills in where, once
   per request: the caller reconnects. The app cannot do it itself because
   the connection belongs to the main loop, and pretending otherwise would
   mean two owners for one socket. */
int app_take_hub_switch(app *a, app_hub_target *to);

/* The favourites, so the caller can find the auto-connect one before the
   first connection is made. */
favs *app_favs(app *a);

/* Switching hubs went wrong, or went right. Both belong on screen: a
   client that silently fails to connect to the hub somebody just picked
   looks broken rather than unlucky. */
void app_hub_failed(app *a, const char *why);
void app_note(app *a, const char *text);
void app_free(app *a);

/* Install the callbacks that feed the UI. Call after app_init. */
void app_attach(app *a, hubconn *h);

/* One pass: read keys, redraw if needed. Returns 0 to keep going, 1 when
   the user has asked to quit. */
int app_tick(app *a, unsigned long now_secs);

/* Show a transient message on the status line. */
void app_status(app *a, const char *msg);

/* Append to the chat pane from outside (startup notices, errors). */
void app_say_local(app *a, sb_kind kind, const char *text);

/* The splash door, shown while connecting. Draw-only: the caller owns the
   wait, because it must keep polling the network and the keyboard rather
   than sleeping. Call app_splash_done() to force a full repaint after. */
void app_splash_draw(app *a);

/* Keep the audio moving from a path that is not running app_tick.
 *
 * The main loop has three of those -- the splash, the sign-off hold, and
 * anything that blocks -- and for each of them the sound card goes on
 * reading its ring whether or not anybody refills it. A starved auto-init
 * ring replays stale audio rather than falling silent, so this is the
 * difference between music and a 1.5-second loop of whatever was in the
 * buffer. */
void app_audio_tick(app *a);

/* One pass of the splash while it is up: animate it and service the audio.
   Call it every time round the loop; it throttles its own redraws. */
void app_splash_tick(app *a, unsigned long now_ms);
void app_splash_done(app *a);

/* The sign-off door: carrier drop, session summary, NO CARRIER. Draws and
   returns; the caller decides how long to leave it up, because it must keep
   servicing the sound queue while it is shown. */
void app_logoff_draw(app *a);

/* Fill the search form and run it. For --search, and for the /search
   command. */
void app_search(app *a, const char *terms);

/* Start scanning and hashing the configured share. Safe to call again --
   it restarts from scratch, which is what /rescan means. */
void app_share_start(app *a);

/* Pause or resume hashing (H in the share door, /hash). Pausing
   checkpoints: whatever is hashed so far is written to the cache, published
   in files.xml and reported to the hub, so stopping never throws work
   away. Returns the new paused state. */
int app_hash_toggle(app *a);

/* Advance the scan and the hasher by one slice. Called from the main loop
   alongside app_tick, because the work has to happen while the UI stays
   responsive and the hub keeps being serviced. */
void app_share_tick(app *a);

/* Accept inbound peer connections and drive every live transfer. Called
   from the main loop beside app_share_tick. */
void app_xfer_tick(app *a);

/* A peer sent us a CTM: dial them. Registered with hub_set_ctm_handler,
   which calls it with the app as `user`. */
void app_on_ctm(void *user, const char *host, int port, const char *token);

/* Fetch a node's file list and show it in the browse door. The fetch goes
   through the transfer queue, so it asks over the hub, matches the
   connection by token and times out exactly as a download does. */
void app_browse_start(app *a, adc_sid sid, const char *nick);

/* Walk to this directory as the listings arrive: "/Share/warez/". Useful
   on its own -- a browse that opens where you left off rather than at the
   top -- and it is how the DOS rig opens a directory, since AUTOTYPE
   cannot press a key inside a running program. */
void app_browse_goto(app *a, const char *path);

/* Queue the selected search result, or the tagged ones. */
void app_queue_selected(app *a);

#endif /* DOSYPP_APP_H */
