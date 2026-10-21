/* test_favs.c - the favourites list.
 *
 * This file holds passwords, which makes two kinds of bug worse than
 * usual: one that loses an entry, and one that hands the wrong password to
 * the wrong hub. So the cases here are mostly about identity (is this the
 * same hub I already have?) and about surviving a file somebody has edited
 * by hand, since that is what a plain-text format invites.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/adc/favs.h"

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                                              \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want) do {                                          \
        g_run++;                                                           \
        if (!(got) || strcmp((got), (want)) != 0) {                        \
            printf("FAIL line %d: got \"%s\" want \"%s\"\n",               \
                   __LINE__, (got) ? (got) : "(null)", (want));            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static char g_path[384];

static void write_raw(const char *text)
{
    FILE *f = fopen(g_path, "wb");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_split(void)
{
    char host[64];
    int port;

#define SPLIT(a) favs_split((a), host, sizeof host, &port)

    CHECK(SPLIT("adc://hub.example:1511") == 0, "scheme and port");
    CHECK_STR(host, "hub.example");
    CHECK(port == 1511, "port is %d", port);

    /* No port: the ADC default, not zero. */
    CHECK(SPLIT("adc://hub.example") == 0, "no port");
    CHECK_STR(host, "hub.example");
    CHECK(port == 1511, "port defaulted to %d", port);

    /* No scheme, which is how people type it. */
    CHECK(SPLIT("hub.example:2780") == 0, "no scheme");
    CHECK_STR(host, "hub.example");
    CHECK(port == 2780, "port is %d", port);

    /* A trailing path, which public lists carry. */
    CHECK(SPLIT("adc://hub.example:2780/") == 0, "trailing slash");
    CHECK_STR(host, "hub.example");
    CHECK(port == 2780, "port is %d", port);

    /* A dotted quad. */
    CHECK(SPLIT("adc://10.0.2.2:1512") == 0, "dotted quad");
    CHECK_STR(host, "10.0.2.2");
    CHECK(port == 1512, "port is %d", port);

    /* Nonsense must be refused rather than producing an empty host that
       something later tries to dial. */
    CHECK(SPLIT("") != 0, "an empty address was accepted");
    CHECK(SPLIT("adc://") != 0, "a scheme with no host was accepted");

    /* A port that is not a number is part of the host, not a port: that is
       what a v6 address or a typo looks like, and guessing 0 would be
       worse than leaving the default. */
    CHECK(SPLIT("adc://hub.example:notaport") == 0, "odd port");
    CHECK(port == 1511, "a non-numeric port became %d", port);

#undef SPLIT
}

static void test_round_trip(void)
{
    favs a, b;
    fav_entry *e;

    favs_init(&a);

    e = favs_add(&a, "The Attic", "adc://attic.example:1511");
    CHECK(e != NULL, "add failed");
    if (!e) return;
    snprintf(e->nick, sizeof e->nick, "%s", "Phantom");
    snprintf(e->pass, sizeof e->pass, "%s", "hunter2");
    e->autoconnect = 1;

    /* A password with spaces in it: the format has to carry one, because
       people use them and a silently truncated password is a login that
       fails for no visible reason. */
    e = favs_add(&a, "Second Place", "adc://second.example:2780");
    CHECK(e != NULL, "add failed");
    if (e)
        snprintf(e->pass, sizeof e->pass, "%s", "correct horse battery");

    CHECK(a.count == 2, "count is %d", a.count);
    CHECK(a.dirty, "adding did not mark the list dirty");

    CHECK(favs_save(&a, g_path) == 0, "save failed");

    favs_init(&b);
    CHECK(favs_load(&b, g_path) == 2, "loaded %d entries", b.count);
    CHECK(!b.dirty, "a freshly loaded list is marked dirty");

    if (b.count == 2) {
        CHECK_STR(b.list[0].name, "The Attic");
        CHECK_STR(b.list[0].addr, "adc://attic.example:1511");
        CHECK_STR(b.list[0].nick, "Phantom");
        CHECK_STR(b.list[0].pass, "hunter2");
        CHECK(b.list[0].autoconnect == 1, "the auto flag was lost");

        CHECK_STR(b.list[1].name, "Second Place");
        CHECK_STR(b.list[1].pass, "correct horse battery");
        CHECK(b.list[1].autoconnect == 0, "the auto flag spread");
    }
}

/* The same hub added twice must stay one entry -- and must keep the
   password that was set on it. Losing a password because a hub list
   spelled the address differently is the bug this guards. */
static void test_identity(void)
{
    favs f;
    fav_entry *e;

    favs_init(&f);

    e = favs_add(&f, "Attic", "adc://attic.example:1511");
    CHECK(e != NULL, "add failed");
    if (!e) return;
    snprintf(e->pass, sizeof e->pass, "%s", "secret");

    /* The same hub, spelled four ways. */
    CHECK(favs_add(&f, "Attic again", "adc://attic.example:1511") == e,
          "the same address made a second entry");
    CHECK(favs_add(&f, "Attic", "attic.example:1511") == e,
          "no scheme made a second entry");
    CHECK(favs_add(&f, "Attic", "ADC://Attic.Example:1511") == e,
          "different case made a second entry");
    CHECK(favs_add(&f, "Attic", "adc://attic.example:1511/") == e,
          "a trailing slash made a second entry");
    CHECK(f.count == 1, "count is %d, expected 1", f.count);
    CHECK_STR(f.list[0].pass, "secret");

    /* A different port is a different hub. */
    CHECK(favs_add(&f, "Elsewhere", "adc://attic.example:2780") != e,
          "a different port was treated as the same hub");
    CHECK(f.count == 2, "count is %d, expected 2", f.count);

    /* And finding it works the same way. */
    CHECK(favs_find(&f, "attic.example:1511") == &f.list[0],
          "find does not match the way add does");
    CHECK(favs_find(&f, "nowhere.example") == NULL,
          "find matched a hub that is not there");
}

static void test_autoconnect(void)
{
    favs f;
    int i;

    favs_init(&f);
    for (i = 0; i < 3; i++) {
        char addr[64];

        snprintf(addr, sizeof addr, "adc://h%d.example", i);
        favs_add(&f, addr, addr);
    }

    CHECK(favs_autoconnect(&f) == NULL, "something is automatic already");

    favs_set_autoconnect(&f, 1);
    CHECK(favs_autoconnect(&f) == &f.list[1], "the wrong one is automatic");

    /* Only one: setting another has to clear the first, or startup would
       have to choose and there is no sensible way to. */
    favs_set_autoconnect(&f, 2);
    CHECK(f.list[1].autoconnect == 0, "two hubs are automatic");
    CHECK(favs_autoconnect(&f) == &f.list[2], "the wrong one is automatic");

    /* Pressing it again turns it off, so there is a way back to not
       connecting to anything. */
    favs_set_autoconnect(&f, 2);
    CHECK(favs_autoconnect(&f) == NULL, "it cannot be turned off");

    /* Removing an entry shifts the rest down rather than leaving a hole. */
    favs_set_autoconnect(&f, 2);
    CHECK(favs_remove(&f, 0) == 1, "remove failed");
    CHECK(f.count == 2, "count is %d after a remove", f.count);
    CHECK(favs_autoconnect(&f) == &f.list[1],
          "the automatic entry moved to the wrong place");
    CHECK(favs_remove(&f, 9) == 0, "removing an index that is not there");
}

/* A plain-text file invites hand editing, so it has to survive one. */
static void test_hand_edited(void)
{
    favs f;

    /* A missing file is a first run, not an error. */
    favs_init(&f);
    CHECK(favs_load(&f, "/nonexistent/DOSYPP.FAV") == 0,
          "a missing file was an error");
    CHECK(f.count == 0, "a missing file produced entries");

    /* Comments, blank lines, odd spacing, keys we do not know. */
    write_raw("; mine\n"
              "\n"
              "[hub]\n"
              "   name   =   Spaced Out   \n"
              "address=adc://a.example:1511\n"
              "colour = green\n"
              "password = \n"
              "auto = YES\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 1, "loaded %d entries", f.count);
    if (f.count == 1) {
        CHECK_STR(f.list[0].name, "Spaced Out");
        CHECK_STR(f.list[0].addr, "adc://a.example:1511");
        CHECK_STR(f.list[0].pass, "");
        /* "YES" is not "no", so it is on: a file saying YES clearly means
           yes, and refusing it would be pedantry. */
        CHECK(f.list[0].autoconnect == 1, "auto = YES was not honoured");
    }

    /* An entry with no address is no use and must not be offered as a hub
       that cannot be dialled. */
    write_raw("[hub]\nname = Nowhere\n\n"
              "[hub]\nname = Somewhere\naddress = adc://b.example\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 1,
          "an entry with no address was kept (%d entries)", f.count);
    if (f.count == 1)
        CHECK_STR(f.list[0].name, "Somewhere");

    /* An address with no name gets the address as its name, rather than an
       empty row nobody can identify. */
    write_raw("[hub]\naddress = adc://c.example:1511\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 1, "loaded %d", f.count);
    if (f.count == 1)
        CHECK_STR(f.list[0].name, "adc://c.example:1511");

    /* Lines before any [hub] header, which is what a half-deleted entry
       leaves. Skipped, not crashed on. */
    write_raw("name = orphan\npassword = lost\n"
              "[hub]\nname = Real\naddress = adc://d.example\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 1, "loaded %d", f.count);
    if (f.count == 1)
        CHECK_STR(f.list[0].name, "Real");

    /* Two entries claiming to be automatic: the first wins. Refusing to
       start over a hand-edit would be the wrong answer. */
    write_raw("[hub]\nname = A\naddress = adc://a.example\nauto = yes\n"
              "[hub]\nname = B\naddress = adc://b.example\nauto = yes\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 2, "loaded %d", f.count);
    if (f.count == 2) {
        CHECK(f.list[0].autoconnect == 1, "the first was not kept");
        CHECK(f.list[1].autoconnect == 0, "both are still automatic");
    }

    /* Nonsense that is not the format at all. */
    write_raw("<html><body>not a favourites file</body></html>\n");
    favs_init(&f);
    CHECK(favs_load(&f, g_path) == 0, "junk produced %d entries", f.count);
}

static void test_cap(void)
{
    favs f;
    FILE *fp;
    int i;

    /* More entries than the cap, from a file. Over the cap they are
       dropped with a reason rather than overrunning the array. */
    fp = fopen(g_path, "wb");
    CHECK(fp != NULL, "cannot write the fixture");
    if (!fp)
        return;
    for (i = 0; i < FAVS_MAX + 10; i++)
        fprintf(fp, "[hub]\nname = H%d\naddress = adc://h%d.example\n",
                i, i);
    fclose(fp);

    favs_init(&f);
    CHECK(favs_load(&f, g_path) == FAVS_MAX,
          "loaded %d of %d, expected the cap of %d",
          f.count, FAVS_MAX + 10, FAVS_MAX);
    CHECK(f.err[0] != '\0', "the cap was hit silently");

    /* And adding past it fails rather than overwriting something. */
    CHECK(favs_add(&f, "One More", "adc://more.example") == NULL,
          "added past the cap");
    CHECK(f.count == FAVS_MAX, "count grew past the cap to %d", f.count);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    printf("DOSy++ favourites tests (addresses / files / passwords)\n");

    snprintf(g_path, sizeof g_path, "%s/dosypp_favs_test.fav",
             tmp && tmp[0] ? tmp : "/tmp");
    remove(g_path);

    test_split();
    test_round_trip();
    test_identity();
    test_autoconnect();
    test_hand_edited();
    test_cap();

    remove(g_path);

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
