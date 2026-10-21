/* main.c - Phase 2: log into a hub, hold a conversation, list who is on.
 *
 * Still a console client: output is printf, input is a line at a time. The
 * BBS interface is Phase 3, and it will consume exactly this state machine
 * through the same callbacks -- which is the point of keeping hubconn free
 * of any display logic.
 *
 *   DOSYPP [-p port] [-n nick] [-w password] [-c dosypp.ini]
 *          [--keepalive secs] [host]
 *
 * Commands once connected:
 *   /who            list users
 *   /pm <nick> ...  private message
 *   /me <action>    third-person chat
 *   /raw <line>     send a raw ADC line
 *   /info           session details
 *   /quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef DOSYPP_WATT32
#  include <dos.h>      /* delay() */
#endif

#include "common.h"
#include "cfg.h"
#include "adc/hubconn.h"
#include "adc/favs.h"
#include "adc/peerconn.h"
#include "adc/ident.h"
#include "net/netinit.h"
#include "net/transport.h"
#include "snd/mus.h"
#include "ui/app.h"
#include "ui/vga.h"
#include "ui/screen.h"
#include "ui/kbd.h"
#include "ui/ansi.h"
#include "snd/pcspkr.h"

#ifdef DOSYPP_WATT32
#  include <conio.h>              /* kbhit/getch: DOS has no select on stdin */
#else
#  include <unistd.h>
#  include <sys/select.h>
#endif

#define CFG_DEFAULT "DOSYPP.INI"

/* strncpy does not NUL-terminate when it truncates. Every use here is a
   deliberate clamp of a config string into a protocol-sized field, so be
   explicit about it rather than relying on the destination having been
   zeroed earlier. */
/* A millisecond-ish pause. delay() is a DOS libc call, and the keyboard
   test compiles on both targets even though it can only do anything on
   DOS. */
static void tiny_delay(void)
{
#ifdef DOSYPP_WATT32
    delay(1);
#else
    net_yield();
#endif
}

static void clamp_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);

    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Diagnostics go to stdout, not stderr: DOS's command.com cannot redirect
   stderr separately (no 2>&1), so anything written there vanishes the moment
   output is redirected to a file -- which is precisely when the message
   matters. */

/* Hub text is never printed raw, not even by the plain console front end.
 *
 * This was a real hole: the UI stripped escape sequences but --console did
 * not, so a hostile MOTD could move the cursor, clear the screen, or set a
 * terminal's answerback string purely by choosing the less fancy front end.
 * The boundary belongs on the data, not on the renderer.
 *
 * UTF-8 is left intact here -- a terminal can display it, unlike an 80x25
 * CP437 screen -- so this only removes escape sequences and @X codes. */
static const char *safe(const char *src)
{
    static char buf[4096];

    ansi_strip(buf, sizeof buf, src ? src : "");
    return buf;
}

/* ------------------------------------------------------------- display */

static const char *class_of(const adc_user *u)
{
    if (!u)
        return "hub";
    if (u->ct & ADC_CT_HUB)
        return "hub";
    if (u->ct & (ADC_CT_OWNER | ADC_CT_SUPER | ADC_CT_OP))
        return "op";
    if (u->ct & ADC_CT_BOT)
        return "bot";
    return "user";
}

/* Human-readable size without floating point -- the whole program is built
   -msoft-float, so this is not a style choice. */
static void fmt_size(tiger_u64 bytes, char *out, size_t outsz)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    tiger_u64 v = bytes;
    unsigned int u = 0;
    unsigned long frac = 0;

    while (v >= 1024 && u < ARRAY_SIZE(unit) - 1) {
        frac = (unsigned long)((v % 1024) * 10 / 1024);
        v /= 1024;
        u++;
    }

    if (u == 0)
        snprintf(out, outsz, "%lu %s", (unsigned long)v, unit[u]);
    else
        snprintf(out, outsz, "%lu.%lu %s", (unsigned long)v, frac, unit[u]);
}

static void on_state(void *user, hub_state s)
{
    (void)user;
    printf("[state] %s\n", hub_state_name(s));
    fflush(stdout);
}

static void on_chat(void *user, const adc_user *from, const char *text, int me)
{
    (void)user;
    if (!from) {
        printf("* %s\n", safe(text));
    } else if (strncmp(text, "/me ", 4) == 0) {
        printf("* %s", safe(from->nick));
        printf(" %s\n", safe(text + 4));
    } else {
        printf("<%s%s>", safe(from->nick), me ? "*" : "");
        printf(" %s\n", safe(text));
    }
    fflush(stdout);
}

static void on_private(void *user, const adc_user *from, const char *text)
{
    (void)user;
    printf("[pm from %s]", safe(from ? from->nick : "?"));
    printf(" %s\n", safe(text));
    fflush(stdout);
}

static void on_join(void *user, const adc_user *u)
{
    hubconn *h = (hubconn *)user;

    /* Stay quiet during the login flood: a 5,000-user hub would otherwise
       scroll 5,000 join lines before the first prompt. */
    if (h && h->state == HUB_NORMAL)
        printf("--> %s joined (%s)\n", safe(u->nick), class_of(u));
}

static void on_part(void *user, adc_sid sid, const char *nick,
                    const char *reason)
{
    (void)user;
    if (nick[0]) {
        printf("<-- %s left", safe(nick));
        printf("%s%s\n", reason[0] ? ": " : "", safe(reason));
    }
    else
        printf("<-- a user left (sid %lu)\n", (unsigned long)sid);
}

static void on_status(void *user, int severity, int code, const char *desc)
{
    static const char *sev[] = { "info", "warn", "FATAL" };

    (void)user;
    printf("[%s %d] %s\n",
           (severity >= 0 && severity <= 2) ? sev[severity] : "?",
           code, safe(desc));
    fflush(stdout);
}

static void print_who(hubconn *h)
{
    unsigned int i = 0;
    const adc_user *u;
    char size[32];
    char total[32];

    printf("\n%-20s %-6s %-10s %5s  %s\n",
           "NICK", "CLASS", "SHARE", "SLOTS", "CLIENT");
    printf("---------------------------------------------------------------\n");

    while ((u = adc_userlist_at(&h->users, i)) != NULL) {
        fmt_size(u->share, size, sizeof size);
        {
            char n[ADC_NICK_MAX * 2], t[ADC_TAG_MAX * 2];
            snprintf(n, sizeof n, "%s",
                     u->nick[0] ? safe(u->nick) : "(no nick)");
            snprintf(t, sizeof t, "%s", safe(u->tag));
            printf("%-20s %-6s %-10s %5u  %s\n",
                   n, class_of(u), size, u->slots, t);
        }
        i++;
    }

    fmt_size(adc_userlist_total_share(&h->users), total, sizeof total);
    printf("---------------------------------------------------------------\n");
    printf("%u user(s), %s shared\n\n", h->users.count, total);
    fflush(stdout);
}

static void print_info(hubconn *h, const cfg *c)
{
    char sid_text[ADC_SID_CHARS + 1];
    const adc_user *self = hub_self(h);

    adc_sid_encode(h->sid, sid_text);
    printf("\nhub      : %s:%d  %s\n", c->hub, c->port,
           h->hubname[0] ? safe(h->hubname) : "(unnamed)");
    printf("state    : %s\n", hub_state_name(h->state));
    printf("nick     : %s\n", h->nick);
    printf("sid      : %s (%lu)\n", sid_text, (unsigned long)h->sid);
    printf("cid      : %s\n", h->id.cid_b32);
    printf("users    : %u\n", h->users.count);
    printf("own inf  : %s\n", self ? "acknowledged" : "not yet seen");
    if (h->err[0])
        printf("last err : %s\n", h->err);
    printf("\n");
    fflush(stdout);
}

/* --------------------------------------------------------------- input */

/* Collect a line of input without blocking. Returns 1 when a complete line
   is ready in buf. DOS has no select() for the console, so the two paths
   genuinely differ. */
static int poll_line(char *buf, size_t bufsz, size_t *fill)
{
#ifdef DOSYPP_WATT32
    while (kbhit()) {
        int ch = getch();

        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            buf[*fill] = '\0';
            *fill = 0;
            return 1;
        }
        if (ch == 8 || ch == 127) {            /* backspace */
            if (*fill > 0) {
                (*fill)--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (ch == 27) {                        /* escape clears the line */
            while (*fill > 0) {
                (*fill)--;
                printf("\b \b");
            }
            fflush(stdout);
            continue;
        }
        if (ch >= 32 && ch < 127 && *fill + 1 < bufsz) {
            buf[(*fill)++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
    return 0;
#else
    struct timeval tv;
    fd_set r;

    FD_ZERO(&r);
    FD_SET(0, &r);
    tv.tv_sec = 0;
    tv.tv_usec = 20000;

    if (select(1, &r, NULL, NULL, &tv) > 0) {
        if (fgets(buf, (int)bufsz, stdin) == NULL)
            return 0;
        buf[strcspn(buf, "\r\n")] = '\0';
        *fill = 0;
        return 1;
    }
    return 0;
#endif
}

/* Returns 0 to keep going, 1 to quit. */
static int handle_command(hubconn *h, const cfg *c, char *line)
{
    if (strcmp(line, "/quit") == 0 || strcmp(line, "/q") == 0)
        return 1;

    if (strcmp(line, "/who") == 0) {
        print_who(h);
        return 0;
    }

    if (strcmp(line, "/info") == 0) {
        print_info(h, c);
        return 0;
    }

    if (strncmp(line, "/raw ", 5) == 0) {
        if (hub_raw(h, line + 5) != 0)
            printf("[error] %s\n", hub_error(h));
        return 0;
    }

    if (strncmp(line, "/pm ", 4) == 0) {
        char *nick = line + 4;
        char *text = strchr(nick, ' ');
        adc_user *target;

        if (!text) {
            printf("[usage] /pm <nick> <message>\n");
            return 0;
        }
        *text++ = '\0';

        target = adc_userlist_find_nick(&h->users, nick);
        if (!target) {
            printf("[error] no such user: %s\n", nick);
            return 0;
        }
        if (hub_pm(h, target->sid, text) != 0)
            printf("[error] %s\n", hub_error(h));
        else
            printf("[pm to %s] %s\n", target->nick, text);
        return 0;
    }

    if (strncmp(line, "/me ", 4) == 0) {
        if (hub_say(h, line) != 0)
            printf("[error] %s\n", hub_error(h));
        return 0;
    }

    if (line[0] == '/') {
        printf("[error] unknown command. "
               "/who /pm /me /raw /info /quit\n");
        return 0;
    }

    if (line[0] != '\0' && hub_say(h, line) != 0)
        printf("[error] %s\n", hub_error(h));
    return 0;
}

/* ---------------------------------------------------------------- main */

/* The hub session and the UI live in static storage, not on main's stack.
 *
 * Between them they are around 150KB, and DJGPP's default stack is 512KB --
 * so a few deep call chains away from a 150KB frame is not a margin worth
 * relying on, and a stack overflow on DOS is a silent reboot rather than a
 * diagnosable fault. An earlier version of the search table made these
 * 800KB, which would have been fatal on the target and produced a
 * reproducible segfault on the host.
 *
 * There is exactly one of each, for the lifetime of the program, so static
 * costs nothing. */
static hubconn h;
static app ui;

/* ------------------------------------------------------- the loop tracer
 *
 * The music rig measured a gap of about a second and a half between frames
 * the mixer produced and frames the card consumed -- appearing once, early
 * in a hashing run, and never growing after that. Something blocks this
 * loop for longer than the ring holds, and guessing which of six calls it
 * is has a one-in-six chance of being right.
 *
 * So: --loop-trace times each phase of a pass and reports any pass that
 * takes longer than a threshold, naming the phase that owned it. Printed as
 * it happens rather than summarised at the end, because the interesting
 * thing is *when* it happens as well as where.
 *
 * clock() is the only sub-second clock both targets have. On DJGPP it is
 * derived from the 8253, which is accurate enough for a tenth of a second;
 * nothing here needs better.
 */
#define TRACE_PHASES 6

static const char *const TRACE_NAME[TRACE_PHASES] = {
    "hub_tick", "hub_poll", "share_tick", "xfer_tick", "app_tick", "yield"
};

static struct {
    int           on;
    unsigned long threshold_ms;         /* report a pass longer than this */
    unsigned long worst_ms[TRACE_PHASES];
    unsigned long total_ms[TRACE_PHASES];
    unsigned long passes;
    unsigned long reported;
    clock_t       mark;
    unsigned long phase_ms[TRACE_PHASES];   /* this pass */
} trace;

static unsigned long ms_since(clock_t from)
{
    clock_t d = clock() - from;

    return (unsigned long)d * 1000UL / (unsigned long)CLOCKS_PER_SEC;
}

static void trace_begin(void)
{
    int i;

    if (!trace.on)
        return;
    for (i = 0; i < TRACE_PHASES; i++)
        trace.phase_ms[i] = 0;
    trace.mark = clock();
}

/* Close off a phase: the time since the last mark belongs to it. */
static void trace_phase(int which)
{
    unsigned long ms;

    if (!trace.on)
        return;

    ms = ms_since(trace.mark);
    trace.mark = clock();

    if (which < 0 || which >= TRACE_PHASES)
        return;

    trace.phase_ms[which] += ms;
    trace.total_ms[which] += ms;
    if (ms > trace.worst_ms[which])
        trace.worst_ms[which] = ms;
}

static void trace_end(unsigned long now)
{
    unsigned long pass = 0;
    int i, worst = 0;

    if (!trace.on)
        return;

    trace.passes++;
    for (i = 0; i < TRACE_PHASES; i++) {
        pass += trace.phase_ms[i];
        if (trace.phase_ms[i] > trace.phase_ms[worst])
            worst = i;
    }

    if (pass >= trace.threshold_ms) {
        trace.reported++;
        printf("LOOP: pass %lu took %lu ms at t=%lu -- %s %lu ms",
               trace.passes, pass, now, TRACE_NAME[worst],
               trace.phase_ms[worst]);
        for (i = 0; i < TRACE_PHASES; i++)
            if (i != worst && trace.phase_ms[i] > 0)
                printf(", %s %lu", TRACE_NAME[i], trace.phase_ms[i]);
        printf("\n");
        fflush(stdout);
    }
}

static void trace_summary(void)
{
    int i;

    if (!trace.on)
        return;

    printf("LOOP SUMMARY: %lu passes, %lu over %lu ms\n",
           trace.passes, trace.reported, trace.threshold_ms);
    for (i = 0; i < TRACE_PHASES; i++)
        printf("LOOP: %-11s worst %5lu ms   total %7lu ms\n",
               TRACE_NAME[i], trace.worst_ms[i], trace.total_ms[i]);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    cfg c;
    const char *cfg_path = CFG_DEFAULT;
    const char *host_arg = NULL;
    const char *nick_arg = NULL;
    const char *pass_arg = NULL;
    int port_arg = 0;
    int batch = 0;
    int keepalive_arg = -1;
    int console = 0;
    const char *theme_arg = NULL;
    const char *shot_path = NULL;
    int shot_door = 0;
    int vgacheck = 0;
    int null_display = 0;
    int run_secs = 0;
    int music_report = 0;
    int rows_arg = 0;
    int refuse_partial = 0;

    /* A hub switch in progress. The dial used to be a blocking call with a
       20-second timeout, which stopped the loop: the music's DMA ring
       drained and the screen froze while a dead host was waited for. It is
       polled now, like the first connect, and the pass that waits still
       services the audio and redraws. */
    int switching = 0;
    unsigned long switch_deadline = 0;
    char switch_nick[FAV_NICK_MAX];
    char switch_pass[FAV_PASS_MAX];
    char switch_label[FAV_NAME_MAX];
    unsigned long music_said = 0;
    const char *browse_arg = NULL;
    const char *browse_dir_arg = NULL;
    int fetch_hublist = 0;
    unsigned long browse_waited = 0;
    int bios_kbd = 0;
    int kbdtest = 0;
    int c_active_arg = 0;
    int c_port_arg = 0;
    const char *art_path = NULL;
    const char *search_arg = NULL;
    int queue_arg = 0;
    int i;

    transport t;
    hub_callbacks cb;
    char input[512];
    size_t fill = 0;
    time_t deadline;
    int rc = 1;
    int have_ui = 0;
    unsigned long splash_until = 0;
    unsigned long shot_deadline = 0;
    int share_started = 0;

    /* Line-buffered: a crash or a full-screen UI takeover must not swallow
       the diagnostics printed just before it. This cost real time once --
       the output simply stopped mid-sentence and the useful lines were
       still sitting in the buffer. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port_arg = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            nick_arg = argv[++i];
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            pass_arg = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cfg_path = argv[++i];
        else if (strcmp(argv[i], "--batch") == 0)
            batch = 1;                  /* log in, report, exit: for tests */
        else if (strcmp(argv[i], "--keepalive") == 0 && i + 1 < argc)
            keepalive_arg = atoi(argv[++i]);   /* seconds; 0 disables */
        else if (strcmp(argv[i], "--console") == 0)
            console = 1;                /* plain scrolling output, no UI */
        else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
            theme_arg = argv[++i];
        else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows_arg = atoi(argv[++i]);     /* 25 or 50, for one run */
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            shot_path = argv[++i];      /* render, write, exit */
        else if (strcmp(argv[i], "--door") == 0 && i + 1 < argc) {
            /* The number you would press, so --door 4 is the door the
               hotkey bar labels "4-XFERS". Taking the raw enum value here
               was a trap: inserting a door shifted every test. */
            shot_door = atoi(argv[++i]) - 1;
            if (shot_door < 0)
                shot_door = 0;
        }
        else if (strcmp(argv[i], "--vgacheck") == 0)
            vgacheck = 1;               /* draw, read 0xB8000 back, report */
        else if (strcmp(argv[i], "--null-display") == 0)
            null_display = 1;           /* fill the buffer, draw nowhere */
        else if (strcmp(argv[i], "--music-report") == 0)
            music_report = 1;           /* print what the card has played */
        else if (strcmp(argv[i], "--refuse-partial") == 0)
            refuse_partial = 1;         /* serve whole lists only */
        else if (strcmp(argv[i], "--loop-trace") == 0) {
            trace.on = 1;
            trace.threshold_ms = 250;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9')
                trace.threshold_ms = (unsigned long)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--run-secs") == 0 && i + 1 < argc)
            run_secs = atoi(argv[++i]); /* run this long before dumping */
        else if (strcmp(argv[i], "--active") == 0) {
            c_active_arg = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                c_port_arg = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--bios-kbd") == 0)
            bios_kbd = 1;               /* skip the int 9h handler */
        else if (strcmp(argv[i], "--kbdtest") == 0)
            kbdtest = 1;                /* inject scancodes, check effect */
        else if (strcmp(argv[i], "--art") == 0 && i + 1 < argc)
            art_path = argv[++i];       /* render splash + logoff, exit */
        else if (strcmp(argv[i], "--search") == 0 && i + 1 < argc)
            search_arg = argv[++i];     /* search once after logging in */
        else if (strcmp(argv[i], "--queue") == 0)
            queue_arg = 1;              /* then queue the first result */
        else if (strcmp(argv[i], "--fetch-hublist") == 0)
            fetch_hublist = 1;          /* fetch the list once, no keys */
        else if (strcmp(argv[i], "--browse") == 0 && i + 1 < argc)
            browse_arg = argv[++i];     /* fetch this node's file list */
        else if (strcmp(argv[i], "--browse-dir") == 0 && i + 1 < argc)
            browse_dir_arg = argv[++i]; /* and open this directory in it */
        else if (strcmp(argv[i], "--peer-trace") == 0)
            peer_trace(1);              /* log client-to-client lines */
        else if (argv[i][0] != '-' && !host_arg)
            host_arg = argv[i];
        else {
            printf(
                "usage: dosypp [options] [host]\n"
                "\n"
                "  -p PORT          hub port\n"
                "  -n NICK          nick to log in with\n"
                "  -w PASSWORD      password, for a registered nick\n"
                "  -c FILE          config file (default DOSYPP.INI)\n"
                "  --console        plain scrolling output, no interface\n"
                "  --theme NAME     renegade | pcboard | amiga\n"
                "  --active [PORT]  accept incoming peer connections\n"
                "  --keepalive N    seconds between keepalives, 0 to disable\n"
                "  --bios-kbd       use the BIOS keyboard, not our int 9h\n"
                "\n"
                "Mostly for the test rigs:\n"
                "  --batch          log in, report, exit\n"
                "  --screenshot F   write the screen to F and exit\n"
                "  --door N         which door to show, 1-10 in hotkey\n"
                "                   order (the hub list is 10, not 0)\n"
                "  --null-display   fill the buffer without a terminal\n"
                "  --rows N         25 or 50 rows, for this run only\n"
                "  --browse-dir P   with --browse, open this directory "
                "(\"/Share/games/\")\n"
                "  --run-secs N     run this long before the screenshot\n"
                "  --music-report   print the music device and frames "
                "played, once a second\n"
                "  --loop-trace [ms]  report any main-loop pass longer "
                "than ms (250 by default)\n"
                "  --refuse-partial serve whole file lists only, as an old "
                "client would\n"
                "  --search WORDS   search once after logging in\n"
                "  --browse NICK    fetch that node's file list\n"
                "  --fetch-hublist  fetch the hub list once, after login\n"
                "  --peer-trace     log every client-to-client line\n"
                "  --queue          queue the first search result\n"
                "  --art FILE       render the splash and sign-off, exit\n"
                "  --vgacheck       verify the renderer against 0xB8000\n"
                "  --kbdtest        inject scancodes and check the UI\n");
            return 2;
        }
    }

    cfg_load(&c, cfg_path);

    /* --batch exists for the automated rigs: open no display, print one
       verdict, exit. Opening the UI would clear the screen and throw the
       verdict away. */
    if (batch)
        console = 1;

    /* A favourite marked for auto-connect decides where we dial, unless a
       host was given on the command line -- somebody naming a hub means
       that hub. The favourite's own handle and password come with it,
       which is the whole point: the account is per hub.
     *
       Loaded here rather than in the app because the first connection
       happens before the UI exists. The app loads it again for the door;
       reading a dozen lines twice costs nothing and keeps the two from
       sharing a half-initialised structure. */
    if (!host_arg) {
        favs startup_favs;
        fav_entry *pick;

        favs_init(&startup_favs);
        favs_load(&startup_favs, FAVS_FILE);
        pick = favs_autoconnect(&startup_favs);

        if (pick) {
            char host[CFG_STR_MAX];
            int port = 0;

            if (favs_split(pick->addr, host, sizeof host, &port) == 0) {
                clamp_copy(c.hub, sizeof c.hub, host);
                c.port = port;
                if (pick->nick[0])
                    clamp_copy(c.nick, sizeof c.nick, pick->nick);
                if (pick->pass[0])
                    clamp_copy(c.password, sizeof c.password, pick->pass);
                printf("favourite: %s\n", pick->name);
            } else {
                printf("favourite \"%s\" has no usable address; "
                       "using the configured hub\n", pick->name);
            }
        }
    }

    if (host_arg)
        clamp_copy(c.hub, sizeof c.hub, host_arg);
    if (port_arg)
        c.port = port_arg;
    if (nick_arg)
        clamp_copy(c.nick, sizeof c.nick, nick_arg);
    if (pass_arg)
        clamp_copy(c.password, sizeof c.password, pass_arg);
    if (c_active_arg)
        c.active = 1;
    if (c_port_arg)
        c.listen_port = c_port_arg;

    printf("DOSy++ %s\n", DOSYPP_VERSION);
    printf("cid: %s\n", c.id.cid_b32);

    /* Configuration problems are printed before anything takes over the
       screen. A silently ignored setting is indistinguishable from a bug in
       the feature it configures. */
    for (i = 0; i < c.nwarn; i++)
        printf("config: %s\n", c.warn[i]);

    /* First run, and nobody has said who they are: ask, the way a board
       would have. One keypress takes the default, which is what makes it
       worth asking at all -- a prompt you have to think about is a worse
       first impression than no prompt.
     *
     * Not when a rig is driving: every one of them passes --batch,
     * --null-display, --screenshot, --art, --kbdtest or --vgacheck, and a
     * question on stdin with nobody to answer it is a hang. The browser
     * demo's GO.BAT passes none of those, which is the case this is for. */
    if (c.dirty && !nick_arg && !batch && !null_display && !shot_path
        && !art_path && !kbdtest && !vgacheck) {
        char answer[32];
        size_t n = 0;
        int done = 0;

        printf("\n");
        printf("Welcome to DOSy++. What shall we call you?\n");
        printf("HANDLE [%s]: ", c.nick);
        fflush(stdout);

        /* Read it with our own keyboard and not with stdin.
         *
         * fgets goes through DOS, and DOS's line input loses characters
         * under v86: in the browser demo the prompt took the first letter
         * typed and nothing after it, whatever the pace. Our own int 9h
         * handler is the path every other key in this program takes, and
         * it reads the same keystrokes correctly -- which is a good enough
         * reason to use it here even without that bug. */
        kbd_open();

        while (!done) {
            int k = kbd_get();

            switch (k) {
            case KEY_NONE:
                net_yield();
                break;
            case KEY_ENTER:
            case 10:                /* a line feed: what a pipe gives us */
                done = 1;
                break;
            case KEY_BACKSPACE:
                if (n > 0) {
                    n--;
                    answer[n] = '\0';
                    printf("\b \b");
                    fflush(stdout);
                }
                break;
            case KEY_ESC:
                /* Clear what has been typed rather than leaving a
                   half-finished handle nobody meant. */
                while (n > 0) {
                    n--;
                    printf("\b \b");
                }
                answer[0] = '\0';
                fflush(stdout);
                break;
            default:
                /* A space would be legal on the wire, escaped, but reads
                   badly in a user list and is awkward to type in /pm.
                   Underscores, as a board would have done. */
                if (k >= ' ' && k < 0x7F && n + 1 < sizeof answer) {
                    answer[n++] = (char)(k == ' ' ? '_' : k);
                    answer[n] = '\0';
                    printf("%c", (int)answer[n - 1]);
                    fflush(stdout);
                }
                break;
            }
        }

        kbd_close();

        if (n > 0)
            clamp_copy(c.nick, sizeof c.nick, answer);

        printf("\n\nHello, %s.\n", c.nick);
    }

    if (c.dirty) {
        /* First run: the identity we just generated has to survive, or the
           next run looks like a different user to every hub. */
        if (cfg_save(&c, cfg_path) == 0)
            printf("wrote %s (new identity)\n", cfg_path);
        else
            printf("warning: could not write %s -- your identity "
                            "will change on the next run\n", cfg_path);
    }

    /* --rows overrides the file for this run only, like --theme: trying a
       taller screen should not rewrite the config to say you wanted it. */
    if (rows_arg > 0)
        c.rows = (rows_arg >= 50) ? 50 : 25;

    /* Render the art and stop. No hub, no network: this exists so the
       splash and the sign-off screen can be iterated on and diffed, which
       is otherwise only possible by quitting a live session. */
    if (art_path) {
        FILE *f = fopen(art_path, "w");

        if (!f) {
            printf("could not write %s\n", art_path);
            return 1;
        }

        vga_open_null(c.rows);
        memset(&h, 0, sizeof h);
        clamp_copy(h.nick, sizeof h.nick, c.nick);
        clamp_copy(h.hubname, sizeof h.hubname, "The Attic");
        h.id = c.id;

        /* --theme overrides the file for this run only: it is a thing you
           try, and overwriting the file because you tried one would be
           rude. */
        if (app_init(&ui, &h, &c, theme_arg ? theme_arg : c.theme) != 0) {
            printf("display: %s\n", vga_lasterr());
            fclose(f);
            return 1;
        }

        fprintf(f, "=== SPLASH ===\n");
        app_splash_draw(&ui);
        vga_dump(&ui.scr, f);

        /* A plausible session, so the summary has something in it. */
        ui.now = 4000;
        ui.connected_at = ui.now - 3725;     /* 1:02:05 */
        ui.nodes_seen = 42;

        fprintf(f, "=== LOGOFF ===\n");
        app_logoff_draw(&ui);
        vga_dump(&ui.scr, f);

        fclose(f);
        app_free(&ui);
        printf("wrote %s\n", art_path);
        return 0;
    }

    if (net_startup() != 0) {
        printf("net: %s\n", net_lasterr());
        return 1;
    }

    printf("connecting to %s:%d as %s ...", c.hub, c.port, c.nick);
    fflush(stdout);

    /* Non-blocking, with visible progress: a blocking connect to a dead host
       freezes for the stack's own timeout, which looks like a hang. */
    {
        int rc = tp_plain_connect_start(&t, c.hub, c.port);
        time_t conn_deadline = time(NULL) + 30;
        time_t last_dot = 0;

        while (rc == TP_CONN_PENDING) {
            if (time(NULL) > conn_deadline) {
                printf(" timed out\n");
                printf("connect: no answer from %s:%d after 30s\n",
                        c.hub, c.port);
                net_shutdown();
                return 1;
            }
            if (time(NULL) != last_dot) {
                last_dot = time(NULL);
                putchar('.');
                fflush(stdout);
            }
            net_yield();
            rc = tp_plain_connect_poll(&t);
        }

        if (rc != TP_CONN_DONE) {
            printf(" failed\n");
            printf("connect: %s\n", tp_plain_lasterr());
            net_shutdown();
            return 1;
        }
        printf(" connected\n");
    }

    memset(&cb, 0, sizeof cb);
    cb.on_state = on_state;
    cb.on_chat = on_chat;
    cb.on_private = on_private;
    cb.on_user_join = on_join;
    cb.on_user_part = on_part;
    cb.on_status = on_status;

    if (hub_init(&h, &t, c.nick, c.password, &c.id, &cb) != 0) {
        printf("hub: %s\n", hub_error(&h));
        goto done;
    }
    cb.user = &h;
    h.cb.user = &h;
    clamp_copy(h.desc, sizeof h.desc, c.desc);
    h.slots = c.slots;
    if (keepalive_arg >= 0)
        h.keepalive_secs = (unsigned int)keepalive_arg;

    /* Login must not hang forever: a hub that accepts the connection and
       then says nothing is a real failure mode. */
    deadline = time(NULL) + 30;

    if (!console) {
        /* The null display fills the cell buffer but never touches a
           terminal, so a screenshot works in CI and over a pipe. It is
           explicit rather than implied by --screenshot, because a keyboard
           test needs the real display AND a dump. */
        if (null_display)
            vga_open_null(c.rows);

        /* --theme overrides the file for this run only: it is a thing you
           try, and overwriting the file because you tried one would be
           rude. */
        if (app_init(&ui, &h, &c, theme_arg ? theme_arg : c.theme) != 0) {
            printf("display: %s\n", vga_lasterr());
            printf("(try --console)\n");
            goto done;
        }
        have_ui = 1;

        /* The share answers other people's searches, so the hub needs a
           reference to it. Borrowed: the app owns the index. */
        hub_set_share(&h, &ui.share);

        /* Active mode, if asked for. Failing to bind is not fatal -- a
           passive client still works for everything except transfers with
           other passive clients -- but it must be reported, because
           "downloads never start" is otherwise unexplainable. */
        if (c.active) {
            if (tp_listen(&ui.listener, c.listen_port) == 0) {
                ui.listening = 1;
                hub_set_active(&h, 1, tp_listen_port(&ui.listener),
                               "0.0.0.0");
                printf("listening on port %d (active mode)\n",
                       tp_listen_port(&ui.listener));
            } else {
                printf("could not listen on port %d: %s\n",
                       c.listen_port, tp_plain_lasterr());
                printf("continuing in passive mode\n");
            }
        }

        /* hubconn calls this with h->cb.user, which app_attach set to the
           app -- so the handler has the real transfer callbacks rather than
           an empty set. */
        hub_set_ctm_handler(&h, app_on_ctm);

        /* The escape hatch: if the int 9h handler misbehaves on hardware we
           have not seen, --bios-kbd gets a usable client back. */
        if (bios_kbd) {
            kbd_close();
            kbd_open_bios();
        }

        app_set_conf_path(&ui, cfg_path);
        ui.xq.no_serve_partial = refuse_partial;
        app_attach(&ui, &h);

        /* --door selects the door for the whole run, not only for the
           screenshot at the end. It used to be applied at dump time alone,
           which meant keystrokes fed to a headless run went to the chat
           input line instead of the door under test -- and the test looked
           like the feature was broken. */
        ui.door = (door_id)shot_door;

        app_splash_draw(&ui);
        splash_until = (unsigned long)time(NULL) + 2;
    }

    while (h.state != HUB_FAILED) {
        int n;
        unsigned long now = (unsigned long)time(NULL);

        trace_begin();

        /* What the sound card has actually played, and what the mixer has
           fed it. At the top of the loop rather than inside the UI branch:
           it used to sit after the hub-switch handling, so during a dial --
           the one time the question "is the loop still running?" matters
           most -- it printed nothing at all. */
        if (have_ui && music_report && now != music_said) {
            music_said = now;
            printf("MUSIC: played=%lu fed=%lu loops=%lu dev=%s\n",
                   mus_frames(), mus_fed(), mus_loops(), mus_device());
            fflush(stdout);
        }

        /* The user picked a hub from the list. The connection belongs to
           this loop, so this is where it gets replaced: say goodbye
           properly, dial the new one, and start the session again.
           Everything the app holds -- the share, the queue, the name map
           -- survives, because none of it is about which hub we are on. */
        if (have_ui && !switching) {
            app_hub_target to;

            if (app_take_hub_switch(&ui, &to)) {
                char msg[128];

                hub_quit(&h);
                hub_free(&h);
                tp_close(&t);

                /* A zeroed hub rather than a freed one: the UI goes on
                   drawing while the dial is in progress, and a half-torn
                   -down connection is not something to draw from. State 0
                   is HUB_PROTOCOL, which is what the doors already show
                   before a login completes. */
                memset(&h, 0, sizeof h);
                app_attach(&ui, &h);

                clamp_copy(c.hub, sizeof c.hub, to.host);
                c.port = to.port;

                /* A favourite's own handle and password, falling back to
                   the config's: a registered account is per hub, and one
                   password in DOSYPP.INI is no use to anyone with two.
                   Copied rather than pointed at: `to` is a local and the
                   dial now spans many passes of this loop. */
                clamp_copy(switch_nick, sizeof switch_nick,
                           to.nick[0] ? to.nick : c.nick);
                clamp_copy(switch_pass, sizeof switch_pass,
                           to.pass[0] ? to.pass : c.password);
                clamp_copy(switch_label, sizeof switch_label,
                           to.label[0] ? to.label : c.hub);

                if (tp_plain_connect_start(&t, c.hub, c.port) ==
                    TP_CONN_FAILED) {
                    snprintf(msg, sizeof msg, "%s:%d is not answering: %s",
                             c.hub, c.port, tp_plain_lasterr());
                    app_hub_failed(&ui, msg);
                    break;
                }

                switching = 1;
                switch_deadline = now + 30;

                snprintf(msg, sizeof msg, "connecting to %s as %s...",
                         switch_label, switch_nick);
                app_note(&ui, msg);
            }
        }

        /* Mid-dial. No hub to tick or poll yet, so this pass does the UI
           and the audio and nothing else -- which is the whole point of
           polling it: a 20-second wait for a dead host used to drain the
           sound card's ring and freeze the screen. */
        if (switching) {
            int rc = tp_plain_connect_poll(&t);
            char msg[128];

            if (rc == TP_CONN_PENDING) {
                if (now > switch_deadline) {
                    snprintf(msg, sizeof msg,
                             "%s:%d did not answer in 30s", c.hub, c.port);
                    app_hub_failed(&ui, msg);
                    break;
                }
                app_tick(&ui, now);
                trace_phase(4);
                net_yield();
                trace_phase(5);
                trace_end(now);
                continue;
            }

            if (rc != TP_CONN_DONE) {
                snprintf(msg, sizeof msg, "%s:%d is not answering: %s",
                         c.hub, c.port, tp_plain_lasterr());
                app_hub_failed(&ui, msg);
                break;
            }

            if (hub_init(&h, &t, switch_nick, switch_pass, &c.id, &cb)
                != 0) {
                snprintf(msg, sizeof msg, "%s", hub_error(&h));
                app_hub_failed(&ui, msg);
                break;
            }
            h.cb.user = &h;
            clamp_copy(h.desc, sizeof h.desc, c.desc);
            h.slots = c.slots;
            if (keepalive_arg >= 0)
                h.keepalive_secs = (unsigned int)keepalive_arg;

            app_attach(&ui, &h);
            hub_set_share(&h, &ui.share);
            hub_set_ctm_handler(&h, app_on_ctm);
            if (ui.listening)
                hub_set_active(&h, 1, tp_listen_port(&ui.listener),
                               "0.0.0.0");

            switching = 0;

            snprintf(msg, sizeof msg, "connected to %s", switch_label);
            app_note(&ui, msg);
        }

        if (hub_tick(&h, now) != 0)
            break;
        trace_phase(0);

        n = hub_poll(&h);
        if (n < 0)
            break;
        trace_phase(1);

        if (have_ui) {
            /* The splash stays up briefly, but the network keeps running
               underneath it -- by the time it clears, login is usually
               already done. */
            if (splash_until) {
                if (now >= splash_until) {
                    splash_until = 0;
                    app_splash_done(&ui);
                } else {
                    /* The splash is not a still image and not a quiet one:
                       this animates the scroller and, more to the point,
                       keeps the sound card's ring fed. Without it the two
                       seconds here were the whole of the gap the music rig
                       kept reporting -- the ring drained, the card went on
                       reading it, and the music started 1.5 seconds behind
                       itself and stayed there. */
                    app_splash_tick(&ui, snd_millis());
                }
            } else {
                /* Scan and hash in slices alongside the UI. Starting it
                   only once logged in means the hub is reachable by the
                   time there is anything to report. */
                if (h.state == HUB_NORMAL && !share_started) {
                    share_started = 1;
                    app_share_start(&ui);
                }
                app_share_tick(&ui);
                trace_phase(2);
                app_xfer_tick(&ui);
                trace_phase(3);
            }

            if (splash_until) {
                /* handled above */
            } else if (app_tick(&ui, now)) {
                trace_phase(4);
                /* Sign off properly: tell the hub, then hold the logoff
                   screen up for a moment so the carrier-drop sound can
                   finish and the summary can be read. The sound queue is
                   serviced throughout, which is why this is a loop and not
                   a sleep. */
                hub_quit(&h);
                app_logoff_draw(&ui);
                {
                    unsigned long until = snd_millis() + 1800;
                    while (snd_millis() < until) {
                        /* Audio, not just the speaker queue: the module is
                           still playing under the sign-off and the ring
                           still needs refilling. */
                        app_audio_tick(&ui);
                        if (kbd_get() != KEY_NONE)
                            break;      /* impatient users exist */
                    }
                }
                rc = 0;
                break;
            }
            trace_phase(4);

            /* Drive the keyboard through the controller and check the UI
               reacted: the only way to test an int 9h handler without a
               pair of hands. */
            if (kbdtest && h.state == HUB_NORMAL && !splash_until) {
                /* '2' selects the WHO door, then two DOWN presses move the
                   lightbar. Make code, then break code, as hardware does. */
                /* Bare 0x50 rather than E0 50: the numeric keypad sends the
                   unprefixed code with Num Lock off, and our decoder accepts
                   both. DOSBox-X's 0xD2 injection does not carry an E0
                   prefix through reliably -- the prefixed form is covered by
                   the host unit tests of kbd_decode instead. */
                /* ...then '3' walks into the file areas door and '4'
                   walks out of it again. That second step is the one
                   worth injecting: with a form field focused, the door
                   used to swallow every digit, so '4' typed a 4 into the
                   search box and there was no way out but F1. A keyboard
                   test that only ever visits doors with no text fields
                   would never have noticed. */
                static const unsigned char SEQ[] = {
                    0x03, 0x83,       /* '2' make, break      -> WHO     */
                    0x50, 0xD0,       /* keypad Down make, break         */
                    0x50, 0xD0,       /* again: lightbar moves           */
                    0x04, 0x84,       /* '3' make, break      -> FILES   */
                    0x05, 0x85,       /* '4' make, break      -> XFER    */
                    0x04, 0x84,       /* '3' again            -> FILES   */
                    0x01, 0x81        /* ESC make, break      -> CHAT    */
                };
                unsigned long got = 0, lost = 0;
                size_t k;
                int ok;
                /* The lightbar is checked where it is made, not at the
                   end: the user list keeps arriving, and a selection past
                   the end of a list that shrank is clamped. Adding two
                   keys to this sequence was enough to turn a passing
                   check into "sel 1", which had nothing to do with the
                   keyboard. */
                int sel_after_downs = -1;
                int who_rows = 0;
                int want_sel;
                /* The door after a digit was pressed while the file areas
                   door had focus, and the door after ESC. Two separate
                   ways out of that door and both were broken: the digit
                   was typed into the search box, and ESC only ever moved
                   the focus back to it. */
                int door_after_digit = -1;

                for (k = 0; k < sizeof SEQ; k++) {
                    unsigned long before = 0, now_ints = 0;
                    int wait;

                    kbd_stats(&before, NULL);

                    if (kbd_inject(SEQ[k]) != 0) {
                        printf("KBDTEST SKIPPED: injection unsupported "
                               "(%s)\n",
                               kbd_have_isr() ? "controller refused"
                                              : "BIOS fallback in use");
                        app_free(&ui);
                        have_ui = 0;
                        rc = 0;
                        goto done;
                    }
                    /* Wait for the interrupt to actually arrive before
                       sending the next byte. net_yield() does not sleep on
                       DOS, so without this we inject faster than the
                       controller delivers and most bytes never raise IRQ1
                       at all -- observed as 3 interrupts from 6 writes. */
                    for (wait = 0; wait < 200; wait++) {
                        kbd_stats(&now_ints, NULL);
                        if (now_ints > before)
                            break;
                        tiny_delay();
                    }

                    app_tick(&ui, now);

                    /* Six bytes in, '2' and two Downs have been seen.
                       The lightbar is read here and not at the end: the
                       user list keeps arriving, and a selection past the
                       end of a list that shrank is clamped. */
                    if (k == 5) {
                        sel_after_downs = ui.who.sel;
                        /* How far two Down presses can go depends on how
                           many users the hub has told us about by now,
                           which is not something the keyboard decides.
                           The old form of this check asserted sel == 2
                           and passed on a race: the list happened to have
                           grown by the time it looked. */
                        who_rows = (int)h.users.count;
                    }
                    if (k == 9)
                        door_after_digit = (int)ui.door;
                }

                app_tick(&ui, now);
                kbd_stats(&got, &lost);

                /* Two things, and they fail differently: the lightbar
                   moved while the WHO door had the keyboard, and the file
                   areas door gave the keyboard back afterwards. */
                /* Two Downs from the top, or the end of the list if it
                   is shorter than that. */
                want_sel = who_rows > 2 ? 2 : (who_rows > 0 ? who_rows - 1
                                                            : 0);
                ok = (sel_after_downs == want_sel)
                     && (door_after_digit == (int)DOOR_XFER)
                     && (ui.door == DOOR_CHAT);
                printf("KBDTEST: driver=%s int9=%lu dropped=%lu "
                       "sel=%d of %d row(s) (wanted %d) "
                       "door after digit=%d (wanted %d) "
                       "door after ESC=%d (wanted %d)\n",
                       kbd_have_isr() ? "int9h" : "BIOS", got, lost,
                       sel_after_downs, who_rows, want_sel,
                       door_after_digit, (int)DOOR_XFER,
                       (int)ui.door, (int)DOOR_CHAT);
                if (ok)
                    printf("KBDTEST OK: two Down presses moved the "
                           "lightbar, and the file areas door let go of "
                           "the keyboard for both a digit and ESC\n");
                else if (sel_after_downs != want_sel)
                    printf("KBDTEST FAILED: the lightbar was at %d after "
                           "two Down presses, not %d (%d row(s) in the "
                           "list)\n", sel_after_downs, want_sel, who_rows);
                else if (door_after_digit != (int)DOOR_XFER)
                    printf("KBDTEST FAILED: a digit pressed on the file "
                           "areas door left us on door %d, not %d -- it "
                           "was typed into the search box instead\n",
                           door_after_digit, (int)DOOR_XFER);
                else
                    printf("KBDTEST FAILED: ESC on the file areas door "
                           "left us on door %d, not %d -- there is no way "
                           "out of that door\n",
                           (int)ui.door, (int)DOOR_CHAT);

                app_free(&ui);
                have_ui = 0;
                rc = ok ? 0 : 1;
                hub_quit(&h);
                goto done;
            }

            /* Read video memory back and confirm the renderer put the right
               bytes there. The only automatable check of the DOS-specific
               draw path. */
            if (vgacheck && h.state == HUB_NORMAL && !splash_until) {
                int bad;
                int vw, vh;

                ui.door = (door_id)shot_door;
                ui.need_redraw = 1;
                app_tick(&ui, now);
                bad = vga_verify(&ui.scr);
                /* Read before app_free: the screen is freed with the app,
                   and reporting its size afterwards printed "0x0". */
                vw = ui.scr.w;
                vh = ui.scr.h;
                app_free(&ui);
                have_ui = 0;
                if (bad < 0)
                    printf("VGACHECK: readback not supported here\n");
                else if (bad == 0)
                    /* The size comes from the screen rather than from
                       SCREEN_W * SCREEN_H: at 50 rows the latter would
                       claim 2,000 cells were checked when 4,000 were. */
                    printf("VGACHECK OK: all %d cells in video memory "
                           "match the buffer (%dx%d)\n", vw * vh, vw, vh);
                else
                    printf("VGACHECK FAILED: %d cells differ\n", bad);
                rc = (bad == 0) ? 0 : 1;
                hub_quit(&h);
                break;
            }

            /* The hub list, for the same reason --search exists: so the
               path can be driven without a keyboard. */
            if (fetch_hublist && h.state == HUB_NORMAL && !splash_until) {
                fetch_hublist = 0;
                app_fetch_hublist(&ui);

                /* On stdout as well as on the door, for the same reason
                   --peer-trace exists: a rig watching a serial port cannot
                   read the screen. What the door shows is the list; what a
                   gate needs to know is how many entries arrived and from
                   where -- the browser demo's list must be the demo's own
                   and not the public service the client defaults to. */
                {
                    int i;

                    printf("hublist: %d hub(s) from %s\n", ui.hubs.count,
                           (ui.conf && ui.conf->hublist[0])
                               ? ui.conf->hublist : HUBLIST_DEFAULT_URL);
                    for (i = 0; i < ui.hubs.count && i < 8; i++)
                        printf("hublist: %s  %s\n", ui.hubs.entries[i].addr,
                               ui.hubs.entries[i].name);
                    if (ui.hubs.err[0])
                        printf("hublist: %s\n", ui.hubs.err);
                    fflush(stdout);
                }

                if (run_secs <= 0)
                    run_secs = 10;
                shot_deadline = 0;
            }

            /* Browse a node by nick, for the same reason --search exists:
               so the path can be driven without a keyboard. */
            if (browse_arg && h.state == HUB_NORMAL && !splash_until) {
                adc_user *who = adc_userlist_find_nick(&h.users,
                                                       browse_arg);

                if (who) {
                    app_browse_start(&ui, who->sid, who->nick);
                    if (browse_dir_arg)
                        app_browse_goto(&ui, browse_dir_arg);
                    browse_arg = NULL;
                    if (run_secs <= 0)
                        run_secs = 10;  /* a list takes a moment to arrive */
                    shot_deadline = 0;
                } else {
                    /* They may not have finished logging in yet. Wait a
                       few seconds before giving up, rather than racing
                       them. */
                    if (!browse_waited)
                        browse_waited = now;
                    else if (now > browse_waited + 8) {
                        printf("no node called %s is here\n", browse_arg);
                        browse_arg = NULL;
                    }
                }
            }

            /* Run the search once, then let the normal wait give the
               results time to arrive before anything is captured. */
            if (search_arg && h.state == HUB_NORMAL && !splash_until) {
                app_search(&ui, search_arg);
                search_arg = NULL;
                if (run_secs <= 0)
                    run_secs = 3;      /* results need a moment */
                shot_deadline = 0;
            }

            /* Queue the first result once some have arrived, so a transfer
               can be driven without a keyboard. */
            if (queue_arg && h.search.count > 0) {
                queue_arg = 0;
                ui.files.sel = 0;
                app_queue_selected(&ui);
                ui.door = (door_id)shot_door;
            }

            /* Screenshot: once logged in and drawn, write the screen and
               stop. Useful on its own and as a layout regression check. */
            if (shot_path && h.state == HUB_NORMAL && !splash_until) {
                FILE *f;

                /* Give the run time to receive keystrokes before dumping:
                   that is how the keyboard path gets tested with AUTOTYPE
                   driving a real DOS session. */
                if (run_secs > 0) {
                    if (!shot_deadline)
                        shot_deadline = now + (unsigned long)run_secs;
                    if (now < shot_deadline) {
                        /* Just round the loop. This used to call app_tick()
                           again before continuing, which ran the UI -- and
                           with it the music mixer -- twice per pass while a
                           --run-secs rig was waiting. The loop that ships
                           runs it once, and a rig that measures a different
                           duty cycle from the real one is worse than no
                           rig: it hid a music tick that could not keep up
                           with the hashing. */
                        net_yield();
                        trace_phase(5);
                        trace_end(now);
                        continue;
                    }
                }

                ui.door = (door_id)shot_door;
                ui.need_redraw = 1;
                app_tick(&ui, now);       /* ensure a draw has happened */
                f = fopen(shot_path, "w");
                if (f) {
                    vga_dump(&ui.scr, f);
                    fclose(f);
                    printf("wrote %s\n", shot_path);
                    {
                        unsigned long got = 0, lost = 0;
                        kbd_stats(&got, &lost);
                        printf("KEYBOARD: %s, %lu int9 fired, "
                               "%lu dropped, door %d\n",
                               kbd_have_isr() ? "int9h" : "BIOS",
                               got, lost, (int)ui.door);
                    }
                    rc = 0;
                } else {
                    printf("could not write %s\n", shot_path);
                }
                hub_quit(&h);
                break;
            }

            if (h.state != HUB_NORMAL && deadline && now > (unsigned long)deadline) {
                snprintf(h.err, sizeof h.err,
                         "timed out in state %s", hub_state_name(h.state));
                break;
            }
            if (h.state == HUB_NORMAL)
                deadline = 0;

            if (n == 0)
                net_yield();
            continue;
        }

        /* ---- console front end ---- */
        if (h.state == HUB_NORMAL) {
            if (batch) {
                printf("\nlogged in to %s as %s\n",
                       h.hubname[0] ? safe(h.hubname) : "(unnamed hub)", h.nick);
                print_who(&h);
                printf("PHASE 2 OK - logged in and have the user list.\n");
                rc = 0;
                break;
            }
            if (deadline) {
                deadline = 0;
                printf("\nlogged in to %s. "
                       "/who /pm /me /raw /info /quit\n\n",
                       h.hubname[0] ? safe(h.hubname) : "(unnamed hub)");
            }

            if (poll_line(input, sizeof input, &fill)) {
                if (handle_command(&h, &c, input)) {
                    hub_quit(&h);
                    rc = 0;
                    break;
                }
            }
        } else if (deadline && time(NULL) > deadline) {
            snprintf(h.err, sizeof h.err,
                     "timed out in state %s", hub_state_name(h.state));
            break;
        }

        if (n == 0)
            net_yield();
        trace_phase(5);
        trace_end(now);
    }

    trace_summary();

    if (have_ui) {
        app_free(&ui);
        have_ui = 0;
    }

    /* Printed after the display has been torn down, so it is still on screen
       when DOSy++ exits -- the UI would otherwise have been restored over
       the top of it. */
    if (rc != 0)
        printf("\nfailed: %s\n", hub_error(&h));

done:
    hub_free(&h);
    tp_close(&t);
    net_shutdown();
    return rc;
}
