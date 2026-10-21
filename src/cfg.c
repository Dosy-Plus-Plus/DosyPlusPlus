/* cfg.c - DOSYPP.INI reader/writer. */
#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static void cfg_warn(cfg *c, const char *fmt, const char *a, int n)
{
    if (c->nwarn >= 4)
        return;
    if (n >= 0)
        snprintf(c->warn[c->nwarn], sizeof c->warn[0], fmt, a, n);
    else
        snprintf(c->warn[c->nwarn], sizeof c->warn[0], fmt, a);
    c->nwarn++;
}

static void set_str(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);

    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int cfg_load(cfg *c, const char *path)
{
    FILE *f;
    char line[256];

    memset(c, 0, sizeof *c);
    set_str(c->hub, sizeof c->hub, "127.0.0.1");
    c->port = 1511;
    set_str(c->nick, sizeof c->nick, "dosypp");
    set_str(c->desc, sizeof c->desc, "");
    c->slots = 1;
    c->sound = 1;
    set_str(c->theme, sizeof c->theme, "renegade");
    c->rows = 25;
    c->music = 1;
    set_str(c->music_file, sizeof c->music_file, "DOSYPP.MOD");
    c->music_rate = 11025;
    set_str(c->music_wav, sizeof c->music_wav, "");
    c->hublist[0] = '\0';          /* the built-in default */
    c->active = 0;              /* passive by default: it always works */
    c->hash_auto = 1;           /* the share is useless until hashed */
    c->rescan_mins = 0;         /* no automatic rescan unless asked */
    c->listen_port = 1412;

    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq, *key, *val;

            trim(line);
            if (line[0] == '\0' || line[0] == ';' || line[0] == '#' ||
                line[0] == '[')
                continue;

            eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            key = line;
            val = eq + 1;
            trim(key);
            trim(val);

            if (strcmp(key, "hub") == 0)
                set_str(c->hub, sizeof c->hub, val);
            else if (strcmp(key, "port") == 0)
                c->port = atoi(val);
            else if (strcmp(key, "nick") == 0)
                set_str(c->nick, sizeof c->nick, val);
            else if (strcmp(key, "password") == 0)
                set_str(c->password, sizeof c->password, val);
            else if (strcmp(key, "description") == 0)
                set_str(c->desc, sizeof c->desc, val);
            else if (strcmp(key, "slots") == 0)
                c->slots = (unsigned int)atoi(val);
            else if (strcmp(key, "active") == 0)
                c->active = (strcmp(val, "yes") == 0 ||
                             strcmp(val, "1") == 0 ||
                             strcmp(val, "on") == 0);
            else if (strcmp(key, "port.listen") == 0)
                c->listen_port = atoi(val);
            else if (strcmp(key, "share") == 0) {
                if (!*val)
                    cfg_warn(c, "ignored an empty 'share =' line", NULL, -1);
                else if (c->nshare >= SHARE_MAX_ROOTS)
                    cfg_warn(c, "too many share paths; ignoring '%s'",
                             val, -1);
                else if (strlen(val) >= SHARE_ROOT_MAX) {
                    char msg[200];
                    snprintf(msg, sizeof msg,
                             "share path is %d chars, limit is %d: %s",
                             (int)strlen(val), SHARE_ROOT_MAX - 1, val);
                    cfg_warn(c, "%s", msg, -1);
                }
                else
                    set_str(c->shares[c->nshare++], SHARE_ROOT_MAX, val);
            }
            else if (strcmp(key, "hash") == 0)
                /* hash = auto | manual. Spelled as a mode rather than a
                   boolean because "hash = no" reads like "never hash",
                   which is not what it does. */
                c->hash_auto = (strcmp(val, "manual") != 0 &&
                                strcmp(val, "no") != 0 &&
                                strcmp(val, "off") != 0);
            else if (strcmp(key, "rescan") == 0) {
                /* Minutes between rescans; 0 means never, and negative is
                   nonsense. There is no sub-minute setting on purpose: a
                   rescan that comes round before the last one finished is
                   a loop, not a schedule. The timer also waits for the
                   previous pass and for any transfer to finish. */
                c->rescan_mins = atoi(val);
                if (c->rescan_mins < 0)
                    c->rescan_mins = 0;
            }
            else if (strcmp(key, "hublist") == 0)
                set_str(c->hublist, sizeof c->hublist, val);
            else if (strcmp(key, "rows") == 0)
                c->rows = (atoi(val) >= 50) ? 50 : 25;
            else if (strcmp(key, "music") == 0)
                c->music = (strcmp(val, "no") != 0 &&
                            strcmp(val, "0") != 0 &&
                            strcmp(val, "off") != 0);
            else if (strcmp(key, "music.file") == 0)
                set_str(c->music_file, sizeof c->music_file, val);
            else if (strcmp(key, "music.wav") == 0)
                set_str(c->music_wav, sizeof c->music_wav, val);
            else if (strcmp(key, "music.rate") == 0) {
                c->music_rate = atoi(val);
                /* An 8-bit card will not take much more than this, and
                   below 8kHz a MOD sounds like a telephone. */
                if (c->music_rate < 8000)
                    c->music_rate = 8000;
                if (c->music_rate > 22050)
                    c->music_rate = 22050;
            }
            else if (strcmp(key, "theme") == 0)
                set_str(c->theme, sizeof c->theme, val);
            else if (strcmp(key, "sound") == 0)
                c->sound = (strcmp(val, "no") != 0 &&
                            strcmp(val, "0") != 0 &&
                            strcmp(val, "off") != 0);
            else if (strcmp(key, "pid") == 0) {
                if (adc_identity_from_b32(&c->id, val) == 0)
                    c->id_valid = 1;
                /* A corrupt pid is deliberately not fatal: generating a new
                   one costs the user their registration on a hub, but
                   refusing to start costs them the client entirely. They are
                   told about it by the caller. */
            }
        }
        fclose(f);
    }

    if (!c->id_valid) {
        adc_identity_generate(&c->id, (unsigned long)c->port ^ 0x5A5A5A5AUL);
        c->id_valid = 1;
        c->dirty = 1;
    }

    if (c->port <= 0 || c->port > 65535)
        c->port = 1511;
    if (c->slots < 1)
        c->slots = 1;

    /* Clamps only. Setting defaults here would discard everything the file
       just said -- which is exactly what happened when these lines were
       duplicated from the block above, and it silently disabled active
       mode and the speaker setting. */
    if (c->listen_port <= 0 || c->listen_port > 65535)
        c->listen_port = 1412;

    return 0;
}

int cfg_save(const cfg *c, const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;

    fprintf(f, "; DOSy++ configuration\n");
    fprintf(f, "; Generated automatically; edit freely.\n\n");
    fprintf(f, "hub         = %s\n", c->hub);
    fprintf(f, "port        = %d\n", c->port);
    fprintf(f, "nick        = %s\n", c->nick);
    fprintf(f, "\n");
    fprintf(f, "; Stored in the clear. This is a plain text file on a DOS\n");
    fprintf(f, "; machine with no file permissions, so treat it as readable\n");
    fprintf(f, "; by anyone who can reach the disk. Leave it empty to be\n");
    fprintf(f, "; prompted with -w instead.\n");
    fprintf(f, "password    = %s\n", c->password);
    fprintf(f, "\n");
    fprintf(f, "description = %s\n", c->desc);
    fprintf(f, "slots       = %u\n", c->slots);
    fprintf(f, "sound       = %s\n", c->sound ? "yes" : "no");
    fprintf(f, "\n");
    fprintf(f, "; Where the hub list comes from. http:// only -- there is\n");
    fprintf(f, "; no TLS in this client yet. Empty uses the built-in\n");
    fprintf(f, "; address, which is a list that exists today.\n");
    fprintf(f, "hublist     = %s\n", c->hublist);
    fprintf(f, "\n");
    fprintf(f, "\n");
    fprintf(f, "; The tune. A missing module or a missing sound card is\n");
    fprintf(f, "; reported once and then ignored. On DOS the card comes\n");
    fprintf(f, "; from BLASTER; on the host the music is written to a WAV\n");
    fprintf(f, "; file, which is how it gets tested.\n");
    fprintf(f, "\n; Screen height: 25, or 50 for the 8x8 font. The doors\n");
    fprintf(f, "; get the extra rows; the layout is the same shape.\n");
    fprintf(f, "rows       = %d\n", c->rows);
    fprintf(f, "\n");
    fprintf(f, "music      = %s\n", c->music ? "yes" : "no");
    fprintf(f, "music.file = %s\n", c->music_file);
    fprintf(f, "music.rate = %d\n", c->music_rate);
    if (c->music_wav[0])
        fprintf(f, "music.wav  = %s\n", c->music_wav);
    fprintf(f, "\n");
    fprintf(f, "; renegade | pcboard | amiga\n");
    fprintf(f, "theme       = %s\n", c->theme);
    fprintf(f, "\n");
    fprintf(f, "; Active mode lets other clients dial you, which is what\n");
    fprintf(f, "; makes transfers possible: two passive clients can never\n");
    fprintf(f, "; connect to each other. It needs the port below reachable.\n");
    fprintf(f, "active      = %s\n", c->active ? "yes" : "no");
    fprintf(f, "port.listen = %d\n", c->listen_port);
    fprintf(f, "\n");
    fprintf(f, "; auto hashes the share as soon as it has been scanned;\n");
    fprintf(f, "; manual waits until you press H in the share door. Either\n");
    fprintf(f, "; way, hashes already in DOSYPP.TTH are used immediately.\n");
    fprintf(f, "hash        = %s\n", c->hash_auto ? "auto" : "manual");
    fprintf(f, "\n");
    fprintf(f, "; Rescan the share every this many minutes. 0 is never.\n");
    fprintf(f, "; Unchanged files come from the hash cache, so a rescan\n");
    fprintf(f, "; that finds nothing new costs seconds, not the share.\n");
    fprintf(f, "; A rescan waits for any transfer to finish first.\n");
    fprintf(f, "rescan      = %d\n", c->rescan_mins);
    fprintf(f, "\n");
    fprintf(f, "; Directories to share. Repeat this key for each one.\n");
    fprintf(f, "; Hashing runs at roughly 500 KB/s on a 486, so a large\n");
    fprintf(f, "; share takes a while the first time -- after that the\n");
    fprintf(f, "; hashes are cached in DOSYPP.TTH and reused.\n");
    {
        int si;
        if (c->nshare == 0)
            fprintf(f, "; share      = C:\\DL\n");
        for (si = 0; si < c->nshare; si++)
            fprintf(f, "share       = %s\n", c->shares[si]);
    }
    fprintf(f, "\n");
    fprintf(f, "; This is your identity on every hub. Keep it: the CID that\n");
    fprintf(f, "; hubs know you by is derived from it, so a new pid looks\n");
    fprintf(f, "; like a different person and loses any registration.\n");
    fprintf(f, "pid         = %s\n", c->id.pid_b32);

    if (fclose(f) != 0)
        return -1;
    return 0;
}
