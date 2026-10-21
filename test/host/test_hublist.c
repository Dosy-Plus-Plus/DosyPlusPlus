/* test_hublist.c - URLs and hub lists, without a network.
 *
 * The fetch itself is exercised end to end by run-hublist-test.sh against a
 * real HTTP server. What is here is the part with the edge cases: splitting
 * a URL, and reading a list that somebody else wrote.
 *
 * A hub list is the one file this client reads that is not from a peer and
 * not from a hub -- it is from a web server somebody else runs -- so the
 * parsing is as defensive as the file-list parser, and for the same reason.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/net/http.h"
#include "../../src/adc/hublist.h"

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

static char g_dir[256];

static void test_urls(void)
{
    char host[128];
    char path[256];
    int port;

#define SPLIT(u) http_split_url((u), host, sizeof host, &port, path, \
                                sizeof path)

    CHECK(SPLIT("http://example.com/list.xml.bz2") == 0, "plain URL");
    CHECK_STR(host, "example.com");
    CHECK(port == 80, "port is %d, expected 80", port);
    CHECK_STR(path, "/list.xml.bz2");

    /* No path: the root, with the leading slash the request needs. */
    CHECK(SPLIT("http://example.com") == 0, "no path");
    CHECK_STR(host, "example.com");
    CHECK_STR(path, "/");

    /* An explicit port. */
    CHECK(SPLIT("http://example.com:8080/hubs") == 0, "port in URL");
    CHECK_STR(host, "example.com");
    CHECK(port == 8080, "port is %d, expected 8080", port);
    CHECK_STR(path, "/hubs");

    /* A query string, which the hublist default uses. */
    CHECK(SPLIT("http://te-home.net/?do=hublist&get=hublist.xml.bz2") == 0,
          "query string");
    CHECK_STR(host, "te-home.net");
    CHECK_STR(path, "/?do=hublist&get=hublist.xml.bz2");

    /* A colon later in the URL must not be read as a port. */
    CHECK(SPLIT("http://example.com/path:with:colons") == 0, "colons");
    CHECK_STR(host, "example.com");
    CHECK(port == 80, "a colon in the path changed the port to %d", port);
    CHECK_STR(path, "/path:with:colons");

    /* A bare host, which is what someone types into a settings field. */
    CHECK(SPLIT("example.com/list") == 0, "no scheme");
    CHECK_STR(host, "example.com");
    CHECK_STR(path, "/list");

    /* https is refused by name rather than attempted: there is no TLS in
       this client, and failing later with a vaguer error would be worse. */
    CHECK(SPLIT("https://example.com/list") != 0, "https was accepted");
    CHECK(strstr(http_error(), "https") != NULL ||
          strstr(http_error(), "TLS") != NULL,
          "the https refusal does not mention why: %s", http_error());

    /* Other schemes, and nonsense. */
    CHECK(SPLIT("ftp://example.com/list") != 0, "ftp was accepted");
    CHECK(SPLIT("http://") != 0, "a URL with no host was accepted");
    CHECK(SPLIT("http://example.com:0/x") != 0, "port 0 was accepted");
    CHECK(SPLIT("http://example.com:99999/x") != 0, "port 99999 accepted");

    /* A host longer than the buffer must be refused, not truncated into
       some other host's name. */
    {
        char big[400];
        int i;

        strcpy(big, "http://");
        for (i = 0; i < 300; i++)
            big[7 + i] = 'a';
        big[307] = '\0';
        CHECK(SPLIT(big) != 0, "an over-long host was accepted");
    }

#undef SPLIT
}

/* A list as a real one looks, including entries this client cannot use. */
static const char *LIST_XML =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Hublist>\n"
    "  <Hubs>\n"
    "    <Hub Name=\"Quiet Hub\" Address=\"adc://quiet.example:1511\" "
         "Description=\"not much going on\" Users=\"3\" "
         "Shared=\"1073741824\"/>\n"
    "    <Hub Name=\"Busy Hub\" Address=\"adc://busy.example:2780\" "
         "Description=\"everyone is here\" Users=\"2500\" "
         "Shared=\"10995116277760\"/>\n"
    "    <Hub Name=\"Middling\" Address=\"adc://mid.example\" "
         "Users=\"120\"/>\n"
    "    <Hub Name=\"Old Protocol\" Address=\"dchub://nmdc.example\" "
         "Users=\"900\"/>\n"
    "    <Hub Name=\"Encrypted\" Address=\"adcs://secure.example:2781\" "
         "Users=\"800\"/>\n"
    "    <Hub Name=\"No Address\" Users=\"50\"/>\n"
    "  </Hubs>\n"
    "</Hublist>\n";

static void write_file(const char *name, const char *text)
{
    char path[384];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_parse(void)
{
    hublist hl;
    char path[384];

    hublist_init(&hl);
    write_file("list.xml", LIST_XML);
    snprintf(path, sizeof path, "%s/list.xml", g_dir);

    CHECK(hublist_load(&hl, path) == 3,
          "kept %d hubs, expected 3 usable ones", hl.count);

    /* Busiest first, because that is the order anyone wants. */
    if (hl.count >= 3) {
        CHECK_STR(hl.entries[0].name, "Busy Hub");
        CHECK(hl.entries[0].users == 2500, "users is %lu",
              hl.entries[0].users);
        CHECK_STR(hl.entries[0].addr, "adc://busy.example:2780");
        CHECK_STR(hl.entries[1].name, "Middling");
        CHECK_STR(hl.entries[2].name, "Quiet Hub");
        CHECK_STR(hl.entries[2].desc, "not much going on");
    }

    /* The ones we cannot use were counted, not silently dropped: a user
       looking at 3 hubs in a list of 6 deserves to know why. */
    CHECK(hl.skipped == 2, "skipped %d, expected 2 (NMDC and ADCS)",
          hl.skipped);

    /* Shared is folded to GB on the way in, because a busy hub's total
       does not fit in 32 bits. */
    if (hl.count >= 1)
        CHECK(hl.entries[0].shared_gb == 10240,
              "shared is %lu GB, expected 10240",
              hl.entries[0].shared_gb);

    /* Not truncated: six entries is not four hundred. */
    CHECK(!hl.truncated, "a six-entry list was reported as truncated");
}

static void test_parse_hostile(void)
{
    hublist hl;
    char path[384];

    snprintf(path, sizeof path, "%s/list.xml", g_dir);

    /* Not XML at all -- which is what an error page from a web server
       looks like, and the usual reason a fetch "works" and gives nothing. */
    hublist_init(&hl);
    write_file("list.xml", "<html><body>404 Not Found</body></html>");
    CHECK(hublist_load(&hl, path) == 0,
          "an HTML error page produced %d hubs", hl.count);

    /* Truncated mid-element. */
    hublist_init(&hl);
    write_file("list.xml",
               "<Hublist><Hubs><Hub Name=\"A\" Address=\"adc://a.example\" "
               "Users=\"5\"/><Hub Name=\"B\" Addre");
    CHECK(hublist_load(&hl, path) >= 1,
          "a truncated list lost the entries before the break");
    CHECK(hl.truncated, "a truncated list was not reported as such");

    /* Absurd values. */
    hublist_init(&hl);
    write_file("list.xml",
               "<Hublist><Hubs>"
               "<Hub Name=\"Liar\" Address=\"adc://a.example\" "
               "Users=\"999999999999999999999\" "
               "Shared=\"999999999999999999999\"/>"
               "</Hubs></Hublist>");
    CHECK(hublist_load(&hl, path) == 1, "a hub with silly numbers was lost");
    /* Saturated rather than wrapped: a wrapped user count would sort a
       dead hub to the top of the list. */
    if (hl.count == 1)
        CHECK(hl.entries[0].users > 1000000UL,
              "a huge user count wrapped to %lu", hl.entries[0].users);

    /* A name with XML entities and a very long description. */
    hublist_init(&hl);
    write_file("list.xml",
               "<Hublist><Hubs>"
               "<Hub Name=\"Bob &amp; Sons\" Address=\"adc://a.example\" "
               "Description=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" Users=\"1\"/>"
               "</Hubs></Hublist>");
    CHECK(hublist_load(&hl, path) == 1, "entities lost the entry");
    if (hl.count == 1) {
        CHECK_STR(hl.entries[0].name, "Bob & Sons");
        CHECK(strlen(hl.entries[0].desc) < HUBLIST_DESC_MAX,
              "the description was not bounded");
    }

    /* An empty file, and a missing one. */
    hublist_init(&hl);
    write_file("list.xml", "");
    CHECK(hublist_load(&hl, path) == 0, "an empty file produced hubs");

    hublist_init(&hl);
    CHECK(hublist_load(&hl, "/nonexistent/hublist.xml") == -1,
          "a missing file was not an error");
    CHECK(hl.err[0] != '\0', "a missing file gave no reason");
}

/* More entries than the cap: the busiest must survive, because the tail of
   a public list is mostly hubs nobody is on. */
static void test_parse_caps(void)
{
    hublist hl;
    char path[384];
    FILE *f;
    int i;

    snprintf(path, sizeof path, "%s/big.xml", g_dir);
    f = fopen(path, "wb");
    CHECK(f != NULL, "cannot write the big fixture");
    if (!f)
        return;

    fputs("<Hublist><Hubs>", f);
    for (i = 0; i < HUBLIST_MAX + 200; i++)
        fprintf(f, "<Hub Name=\"H%d\" Address=\"adc://h%d.example\" "
                   "Users=\"%d\"/>", i, i, i);
    fputs("</Hubs></Hublist>", f);
    fclose(f);

    hublist_init(&hl);
    CHECK(hublist_load(&hl, path) == HUBLIST_MAX,
          "kept %d of a %d-entry list, expected the cap of %d",
          hl.count, HUBLIST_MAX + 200, HUBLIST_MAX);
    CHECK(hl.truncated, "a capped list was not reported as truncated");

    /* The busiest entry in the file is the last one written. */
    if (hl.count > 0)
        CHECK(hl.entries[0].users == (unsigned long)(HUBLIST_MAX + 199),
              "the busiest hub kept has %lu users, expected %d",
              hl.entries[0].users, HUBLIST_MAX + 199);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char cmd[512];

    printf("DOSy++ hub list tests (URLs / parsing / caps)\n");

    snprintf(g_dir, sizeof g_dir, "%s/dosypp_hublist_test",
             tmp && tmp[0] ? tmp : "/tmp");
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) {
        printf("FAILED: cannot create %s\n", g_dir);
        return 1;
    }

    test_urls();
    test_parse();
    test_parse_hostile();
    test_parse_caps();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)system(cmd);

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
