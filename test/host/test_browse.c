/* test_browse.c - bzip2, the file-list parser, and navigating a peer's share.
 *
 * The compression half is checked against the system bzip2 both ways --
 * ours decompressed by theirs, theirs decompressed by ours -- because a
 * round trip through our own code would pass just as happily if we had
 * invented a private format. That is the same reason the hash tests use
 * rhash.
 *
 * The parser half is mostly hostile input. A file list arrives from a
 * stranger, and the failure that matters is not "a name renders oddly" but
 * "a malformed list crashes the client or puts files in the wrong
 * directory".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, for the order check */
#include <unistd.h>

#include "../../src/share/bz.h"
#include "../../src/share/xmlpull.h"
#include "../../src/share/browse.h"
#include "../../src/share/filelist.h"
#include "../../src/share/index.h"

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

static void path_of(char *out, size_t outsz, const char *name)
{
    snprintf(out, outsz, "%s/%s", g_dir, name);
}

static int write_file(const char *name, const char *text)
{
    char path[384];
    FILE *f;

    path_of(path, sizeof path, name);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static int have_bzip2(void)
{
    /* Checked once, so a machine without it skips the oracle cases rather
       than reporting failures that are not ours. */
    return system("bzip2 --help > /dev/null 2>&1") == 0;
}

/* ------------------------------------------------------------ compression */

static void test_bzip2(int oracle)
{
    char src[384], bz[384], back[384], cmd[1024];
    char buf[4096];
    bz_reader *r;
    FILE *f;
    int i, n;

    path_of(src, sizeof src, "plain.txt");
    path_of(bz, sizeof bz, "plain.bz2");
    path_of(back, sizeof back, "back.txt");

    /* Repetitive, like XML: compressible enough that a failure to compress
       at all would be obvious. */
    f = fopen(src, "wb");
    CHECK(f != NULL, "cannot write the source file");
    if (!f)
        return;
    for (i = 0; i < 2000; i++)
        fprintf(f, "<File Name=\"FILE%04d.ZIP\" Size=\"%d\"/>\n", i, i * 7);
    fclose(f);

    CHECK(bz_compress_file(src, bz) == 0, "compression failed: %s",
          bz_error());

    /* Smaller than the input, and not empty. */
    {
        long plain_len, bz_len;

        f = fopen(src, "rb"); fseek(f, 0, SEEK_END);
        plain_len = ftell(f); fclose(f);
        f = fopen(bz, "rb");
        CHECK(f != NULL, "no .bz2 was written");
        if (!f) return;
        fseek(f, 0, SEEK_END); bz_len = ftell(f); fseek(f, 0, SEEK_SET);

        CHECK(bz_len > 14, "the .bz2 is too short to be a stream (%ld)",
              bz_len);
        CHECK(bz_len < plain_len, "compression made it bigger (%ld -> %ld)",
              plain_len, bz_len);

        /* The magic, because a file that is not a bzip2 stream will be
           rejected by every peer and the error will be theirs to see. */
        CHECK(fread(buf, 1, 4, f) == 4, "cannot read the header");
        CHECK(buf[0] == 'B' && buf[1] == 'Z' && buf[2] == 'h' &&
              buf[3] >= '1' && buf[3] <= '9',
              "bad bzip2 magic: %02x %02x %02x %02x",
              buf[0], buf[1], buf[2], buf[3]);
        fclose(f);
    }

    /* Our decompressor, over our compressor. */
    r = bz_open(bz);
    CHECK(r != NULL, "cannot open our own .bz2: %s", bz_error());
    if (r) {
        f = fopen(back, "wb");
        while ((n = bz_read(r, buf, sizeof buf)) > 0) {
            fwrite(buf, 1, (size_t)n, f);
        }
        CHECK(n == 0, "decompression ended with an error: %s", bz_error());
        fclose(f);
        bz_close(&r);
        CHECK(r == NULL, "bz_close did not clear the handle");

        snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s'", src, back);
        CHECK(system(cmd) == 0, "the round trip changed the bytes");
    }

    if (!oracle) {
        printf("note: bzip2 not installed; skipped the cross-checks\n");
        return;
    }

    /* Theirs over ours: proof we produce a real bzip2 stream and not
       something only we can read. */
    snprintf(cmd, sizeof cmd, "bzip2 -t '%s'", bz);
    CHECK(system(cmd) == 0, "bzip2 -t rejects the stream we produced");

    /* Ours over theirs, including a block size we never write ourselves --
       a peer may have compressed with -9 and we have to cope. */
    {
        char theirs[384];

        path_of(theirs, sizeof theirs, "theirs.bz2");
        snprintf(cmd, sizeof cmd, "bzip2 -9 -c '%s' > '%s'", src, theirs);
        CHECK(system(cmd) == 0, "could not run bzip2 to make a fixture");

        r = bz_open(theirs);
        CHECK(r != NULL, "cannot open a stream bzip2 made: %s", bz_error());
        if (r) {
            f = fopen(back, "wb");
            while ((n = bz_read(r, buf, sizeof buf)) > 0)
                fwrite(buf, 1, (size_t)n, f);
            CHECK(n == 0, "failed decompressing bzip2's output: %s",
                  bz_error());
            fclose(f);
            bz_close(&r);

            snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s'", src, back);
            CHECK(system(cmd) == 0,
                  "we decompressed bzip2's stream to the wrong bytes");
        }
    }

    /* Truncation must be an error, not a short read that looks like the
       end of a list. Half a file list is how a peer disconnecting mid
       transfer presents itself. */
    {
        char cut[384];

        path_of(cut, sizeof cut, "cut.bz2");
        snprintf(cmd, sizeof cmd,
                 "head -c 60 '%s' > '%s'", bz, cut);
        CHECK(system(cmd) == 0, "could not make a truncated fixture");

        r = bz_open(cut);
        if (r) {
            int saw_error = 0;

            while ((n = bz_read(r, buf, sizeof buf)) != 0) {
                if (n < 0) { saw_error = 1; break; }
            }
            CHECK(saw_error, "a truncated stream was read as a clean end");
            bz_close(&r);
        }
    }

    /* Something that is not bzip2 at all. */
    {
        char junk[384];

        path_of(junk, sizeof junk, "junk.bz2");
        write_file("junk.bz2", "this is not a compressed file list");
        r = bz_open(junk);
        if (r) {
            n = bz_read(r, buf, sizeof buf);
            CHECK(n < 0, "garbage was accepted as a bzip2 stream");
            bz_close(&r);
        }
    }
}

/* ---------------------------------------------------------- the parser */

/* Feed the parser from a string, so the cases read as what they test. */
typedef struct {
    const char *text;
    size_t pos, len;
} strsrc;

static int fill_str(void *user, void *buf, size_t len)
{
    strsrc *s = (strsrc *)user;
    size_t n = s->len - s->pos;

    if (n == 0)
        return 0;
    if (n > len)
        n = len;
    /* Deliberately dribbles: four bytes at a time exercises the refill
       path, which is where a pull parser breaks. */
    if (n > 4)
        n = 4;
    memcpy(buf, s->text + s->pos, n);
    s->pos += n;
    return (int)n;
}

static long load_xml(browse_list *b, const char *xml)
{
    strsrc s;

    s.text = xml;
    s.pos = 0;
    s.len = strlen(xml);
    return browse_load(b, fill_str, &s, "Peer");
}

static const char *GOOD_LIST =
    "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
    "<FileListing Version=\"1\" CID=\"AAAA\" Base=\"/\" Generator=\"DC++\">\n"
    "  <!-- a comment with a > inside it -->\n"
    "  <Directory Name=\"warez\">\n"
    "    <Directory Name=\"apps\">\n"
    "      <File Name=\"ARCPACK2.EXE\" Size=\"48000\" "
         "TTH=\"ZMOK5JEGGIN2UIMJQIMZ2IHW7VXPU72ZXAA4V4Q\"/>\n"
    "    </Directory>\n"
    "    <File Name=\"MAZE &amp; MAZE2.ZIP\" Size=\"180000\" "
         "TTH=\"4Z6NZGTOMYTQ2D6U2WUQTRFIO2FM2ZYACUZOL2Q\"/>\n"
    "  </Directory>\n"
    "  <Directory Name=\"empty\"/>\n"
    "  <File Name=\"README.TXT\" Size=\"42\" "
         "TTH=\"LWPNACQDBZRYXW3VHJVCJ64QBZNGHOHHHZWCLNQ\"/>\n"
    "</FileListing>\n";

static void test_parse(void)
{
    browse_list b;
    long idx[32];
    long n;

    browse_init(&b);

    n = load_xml(&b, GOOD_LIST);
    CHECK(n == 6, "parsed %ld entries, expected 6", n);
    CHECK(!b.truncated, "a good list was marked truncated: %s",
          browse_error(&b));

    /* The root: two directories then one file, directories first and by
       name within each group. The list has warez before empty, so this
       also says the order comes from the names and not from the order the
       entries arrived in -- which is the order the sharer's filesystem
       returned, and differs between machines. */
    n = browse_listing(&b, idx, 32);
    CHECK(n == 3, "root holds %ld entries, expected 3", n);
    if (n == 3) {
        CHECK_STR(browse_name(&b, idx[0]), "empty");
        CHECK_STR(browse_name(&b, idx[1]), "warez");
        CHECK_STR(browse_name(&b, idx[2]), "README.TXT");
    }

    /* An entity in a name must survive. */
    browse_enter(&b, idx[1]);
    n = browse_listing(&b, idx, 32);
    CHECK(n == 2, "warez holds %ld entries, expected 2", n);
    if (n == 2) {
        CHECK_STR(browse_name(&b, idx[0]), "apps");
        CHECK_STR(browse_name(&b, idx[1]), "MAZE & MAZE2.ZIP");
    }

    {
        char path[128];

        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/warez");
    }

    /* And the names, case-insensitively: a DOS share is upper case and
       somebody else's is not, and a listing that sorts ARJ.ZIP away from
       arachne.zip reads as broken. */
    {
        long i;
        int ordered = 1;

        for (i = 1; i < n; i++) {
            const char *prev = browse_name(&b, idx[i - 1]);
            const char *cur = browse_name(&b, idx[i]);
            if (prev && cur && strcasecmp(prev, cur) > 0)
                ordered = 0;
        }
        CHECK(ordered, "the listing is not in name order");
    }

    /* Down, then back up. */
    browse_enter(&b, idx[0]);
    {
        char path[128];
        char tth[64];
        long inner[8];
        long m = browse_listing(&b, inner, 8);

        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/warez/apps");
        CHECK(m == 1, "apps holds %ld entries, expected 1", m);
        if (m == 1) {
            CHECK_STR(browse_name(&b, inner[0]), "ARCPACK2.EXE");
            CHECK(browse_tth(&b, inner[0], tth, sizeof tth) == 0,
                  "no hash for a file that had one");
            CHECK_STR(tth, "ZMOK5JEGGIN2UIMJQIMZ2IHW7VXPU72ZXAA4V4Q");
        }
    }

    browse_up(&b);
    {
        char path[128];
        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/warez");
    }
    browse_up(&b);
    {
        char path[128];
        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/");
    }

    /* Going up from the root must stay at the root rather than wrapping
       into some other directory. */
    browse_up(&b);
    {
        char path[128];
        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/");
    }

    /* An empty directory still exists and is enterable. In name order
       "empty" comes before "warez", so it is the first row. */
    n = browse_listing(&b, idx, 32);
    browse_enter(&b, idx[0]);
    {
        char path[128];
        long m = browse_listing(&b, idx, 32);

        browse_path(&b, path, sizeof path);
        CHECK_STR(path, "/empty");
        CHECK(m == 0, "an empty directory listed %ld entries", m);
    }

    browse_free(&b);
}

static void test_parse_hostile(void)
{
    browse_list b;
    long n;
    char buf[8192];

    browse_init(&b);

    /* Truncated mid-tag. */
    n = load_xml(&b, "<FileListing><Directory Name=\"a\"><File Name=\"x\" Siz");
    CHECK(b.truncated, "a list cut off mid-tag was accepted as complete");

    /* Truncated between tags, with a directory still open: the files seen
       so far are real, but the list is not all of them. */
    n = load_xml(&b, "<FileListing><Directory Name=\"a\">"
                     "<File Name=\"x\" Size=\"1\" "
                     "TTH=\"LWPNACQDBZRYXW3VHJVCJ64QBZNGHOHHHZWCLNQ\"/>");
    CHECK(n == 2, "kept %ld entries from a truncated list, expected 2", n);
    CHECK(b.truncated, "an unclosed directory was not reported as truncated");

    /* More closing tags than opening ones. This is the one that would put
       files in the wrong directory if it were tolerated. */
    n = load_xml(&b, "<FileListing><Directory Name=\"a\"></Directory>"
                     "</Directory></FileListing>");
    CHECK(b.truncated, "unbalanced tags were accepted");

    /* Absurd nesting. Bounded, and no crash. */
    {
        int i;
        size_t used = 0;

        used += (size_t)snprintf(buf + used, sizeof buf - used,
                                 "<FileListing>");
        for (i = 0; i < 200; i++)
            used += (size_t)snprintf(buf + used, sizeof buf - used,
                                     "<Directory Name=\"d%d\">", i);
        n = load_xml(&b, buf);
        CHECK(b.truncated, "200 levels of nesting was accepted as a share");
        CHECK(n <= 200, "nesting produced %ld entries", n);
    }

    /* A file with no hash is not offerable, so it is dropped rather than
       listed as something that cannot be downloaded. */
    n = load_xml(&b, "<FileListing><File Name=\"nohash.zip\" Size=\"9\"/>"
                     "</FileListing>");
    CHECK(n == 0, "a file with no TTH was listed (%ld entries)", n);

    /* A hash that is not valid base32 must be rejected, not sent back out
       in a CGET. */
    n = load_xml(&b, "<FileListing><File Name=\"bad.zip\" Size=\"9\" "
                     "TTH=\"not!valid!base32\"/></FileListing>");
    CHECK(n == 0, "a file with an unusable TTH was listed (%ld entries)", n);

    /* An over-long name is truncated, not overflowed. */
    {
        char name[600];
        memset(name, 'A', sizeof name - 1);
        name[sizeof name - 1] = '\0';
        snprintf(buf, sizeof buf,
                 "<FileListing><File Name=\"%s\" Size=\"1\" "
                 "TTH=\"LWPNACQDBZRYXW3VHJVCJ64QBZNGHOHHHZWCLNQ\"/>"
                 "</FileListing>", name);
        n = load_xml(&b, buf);
        CHECK(n == 1, "an over-long name lost the file (%ld)", n);
        if (n == 1)
            CHECK(strlen(browse_name(&b, 0)) < XP_NAME_MAX,
                  "a name of %lu bytes got through",
                  (unsigned long)strlen(browse_name(&b, 0)));
    }

    /* Empty input, and input that is not XML at all. */
    n = load_xml(&b, "");
    CHECK(n == 0, "empty input produced %ld entries", n);
    n = load_xml(&b, "this is not xml");
    CHECK(n == 0, "junk produced %ld entries", n);

    browse_free(&b);
}

static void test_unescape(void)
{
    char s[128];

#define UN(in, want) do {                                                  \
        snprintf(s, sizeof s, "%s", in);                                   \
        xp_unescape(s);                                                    \
        CHECK_STR(s, want);                                                \
    } while (0)

    UN("plain", "plain");
    UN("a &amp; b", "a & b");
    UN("&lt;tag&gt;", "<tag>");
    UN("&quot;quoted&quot;", "\"quoted\"");
    UN("it&apos;s", "it's");
    UN("&#65;&#66;", "AB");
    UN("&#x41;&#x42;", "AB");
    /* Not an entity we know: kept verbatim rather than dropped, because a
       mangled name is better than a missing one. */
    UN("100&nbsp;%", "100&nbsp;%");
    UN("&", "&");
    UN("&#;", "&#;");
    /* An ampersand-heavy name must not shift bytes off the end. */
    UN("&amp;&amp;&amp;", "&&&");

#undef UN
}

/* ------------------------------------------- the whole round trip */

/* Our own share -> files.xml -> files.xml.bz2 -> parsed back as if it were
   someone else's. This is the path a peer actually takes, and the one place
   a mismatch between the writer and the reader would show up. */
static void test_round_trip(void)
{
    share_index ix;
    share_file *f;
    browse_list b;
    char xml[384], bz2[384];
    unsigned char tth[TTH_HASH_LEN];
    long idx[16], n;

    path_of(xml, sizeof xml, "ours.xml");
    path_of(bz2, sizeof bz2, "ours.bz2");

    CHECK(share_init(&ix) == 0, "share_init failed");

    f = share_add_file(&ix, "DL/warez/MAZERUN2.ZIP", "C:\\DL\\MAZERUN2.ZIP",
                       180000, 0);
    CHECK(f != NULL, "add failed");
    memset(tth, 0x42, sizeof tth);
    if (f) share_set_tth(&ix, f, tth);

    /* A name with an XML metacharacter, which is where a writer and a
       reader most often disagree. */
    f = share_add_file(&ix, "DL/warez/AT&T <v2>.NFO", "C:\\DL\\ATT.NFO",
                       99, 0);
    CHECK(f != NULL, "add failed");
    memset(tth, 0x43, sizeof tth);
    if (f) share_set_tth(&ix, f, tth);

    CHECK(filelist_publish(&ix, xml, bz2, "MYCIDAAAA") == 2,
          "publishing the list failed");

    browse_init(&b);
    n = browse_load_bz2(&b, bz2, "Me");
    CHECK(n > 0, "could not read back our own list: %s", browse_error(&b));
    CHECK(!b.truncated, "our own list parsed as truncated: %s",
          browse_error(&b));

    /* Descend to the directory the virtual paths describe. */
    n = browse_listing(&b, idx, 16);
    CHECK(n == 1, "root of our own list holds %ld entries", n);
    if (n == 1) {
        browse_enter(&b, idx[0]);
        n = browse_listing(&b, idx, 16);
        CHECK(n == 1, "DL holds %ld entries", n);
        if (n == 1) {
            browse_enter(&b, idx[0]);
            n = browse_listing(&b, idx, 16);
            CHECK(n == 2, "warez holds %ld entries, expected 2", n);
            if (n == 2) {
                /* Both names, including the escaped one, must come back
                   exactly as they went in. */
                const char *a = browse_name(&b, idx[0]);
                const char *c = browse_name(&b, idx[1]);

                CHECK(strcmp(a, "AT&T <v2>.NFO") == 0 ||
                      strcmp(c, "AT&T <v2>.NFO") == 0,
                      "the escaped name did not survive: \"%s\", \"%s\"",
                      a, c);
                CHECK(strcmp(a, "MAZERUN2.ZIP") == 0 ||
                      strcmp(c, "MAZERUN2.ZIP") == 0,
                      "a plain name did not survive: \"%s\", \"%s\"", a, c);
            }
        }
    }

    browse_free(&b);
    share_free(&ix);
}

/* Partial lists, read back: the writer and the reader against each other,
   one directory at a time.

   This is the round trip that matters for browsing a share too big to
   hold: publish a listing of one directory, parse it, walk into the next,
   publish that, merge it, and check the tree that results is the same one
   the whole list would have given. */
static void test_partial_round_trip(void)
{
    share_index ix;
    browse_list b;
    char xml[384], bz2[384];
    unsigned char tth[TTH_HASH_LEN];
    long idx[16], n;
    long dl = -1, warez = -1;

    path_of(xml, sizeof xml, "part.xml");
    path_of(bz2, sizeof bz2, "part.bz2");

    CHECK(share_init(&ix) == 0, "share_init failed");

    {
        static const char *const PATHS[] = {
            "DL/README.TXT", "DL/warez/MAZERUN2.ZIP", "DL/warez/ARCPACK2.EXE",
            "DL/warez/apps/DSKTOOLS.EXE", "DL/mods/TUNE0001.MOD"
        };
        size_t k;

        for (k = 0; k < sizeof PATHS / sizeof PATHS[0]; k++) {
            share_file *f = share_add_file(&ix, PATHS[k], "C:\\X", 100, 0);

            memset(tth, (int)(0x50 + k), sizeof tth);
            if (f)
                share_set_tth(&ix, f, tth);
        }
    }

    /* ---- the root, one level */
    CHECK(filelist_publish_partial(&ix, "/", 0, xml, bz2, "MYCID") == 0,
          "publishing a listing of / failed");

    browse_init(&b);
    n = browse_load_bz2(&b, bz2, "Them");
    CHECK(n == 1, "a listing of / gave %ld entries, expected 1 (DL)", n);
    CHECK(!b.whole, "a partial list was taken for the whole share");

    n = browse_listing(&b, idx, 16);
    CHECK(n == 1, "the root holds %ld entries", n);
    if (n != 1) { browse_free(&b); share_free(&ix); return; }

    dl = idx[0];
    CHECK_STR(browse_name(&b, dl), "DL");
    CHECK(browse_needs_fetch(&b, dl),
          "the share root does not need fetching, so nothing would ask");

    /* The path that would go in the CGET -- with its trailing slash, which
       is what the other side matches on. */
    {
        char path[128];

        browse_entry_path(&b, dl, path, sizeof path);
        CHECK_STR(path, "/DL/");
    }

    /* ---- and the listing of it, merged in */
    CHECK(filelist_publish_partial(&ix, "/DL/", 0, xml, bz2, "MYCID") == 1,
          "publishing a listing of /DL/ failed");

    n = browse_merge_bz2(&b, bz2, dl, "/DL/");
    CHECK(n == 3, "merging /DL/ added %ld entries, expected 3 "
          "(mods, warez, README.TXT)", n);
    CHECK(!browse_needs_fetch(&b, dl),
          "the directory still wants fetching after its listing arrived");

    browse_enter(&b, dl);
    n = browse_listing(&b, idx, 16);
    CHECK(n == 3, "/DL/ holds %ld entries, expected 3", n);
    if (n == 3) {
        /* Directories first, then files -- the door's order. */
        CHECK(b.ents[idx[0]].is_dir && b.ents[idx[1]].is_dir,
              "the directories did not sort first");
        CHECK_STR(browse_name(&b, idx[2]), "README.TXT");
        CHECK(browse_needs_fetch(&b, idx[0]) &&
              browse_needs_fetch(&b, idx[1]),
              "the subdirectories are not marked as needing a fetch");

        warez = strcmp(browse_name(&b, idx[0]), "warez") == 0
                ? idx[0] : idx[1];
    }

    /* ---- one level deeper */
    if (warez >= 0) {
        char path[128];

        browse_entry_path(&b, warez, path, sizeof path);
        CHECK_STR(path, "/DL/warez/");

        CHECK(filelist_publish_partial(&ix, path, 0, xml, bz2, "MYCID") == 2,
              "publishing a listing of %s failed", path);

        n = browse_merge_bz2(&b, bz2, warez, path);
        CHECK(n == 3, "merging %s added %ld entries, expected 3", path, n);

        browse_enter(&b, warez);
        n = browse_listing(&b, idx, 16);
        CHECK(n == 3, "warez holds %ld entries, expected 3", n);

        /* Up and back down, which is what BACKSPACE then ENTER does: the
           entries have to still be there rather than needing a refetch. */
        browse_up(&b);
        n = browse_listing(&b, idx, 16);
        CHECK(n == 3, "going up landed somewhere with %ld entries", n);
        browse_enter(&b, warez);
        n = browse_listing(&b, idx, 16);
        CHECK(n == 3, "coming back down lost the entries (%ld)", n);
    }

    /* ---- a second merge of the same directory changes nothing. Pressing
       ENTER twice must not duplicate a tree, and the entries are indices
       that other entries point at, so appending again would be a mess. */
    if (warez >= 0) {
        long before = b.count;

        CHECK(browse_merge_bz2(&b, bz2, warez, NULL) == 0,
              "a second merge of the same directory added entries");
        CHECK(b.count == before,
              "the table grew from %ld to %ld on a repeat merge",
              before, b.count);
    }

    /* ---- a listing of somewhere else is refused rather than filed under
       the wrong directory. A peer that answers the wrong question would
       otherwise have its files appear in a folder they are not in. */
    if (warez >= 0 && dl >= 0) {
        long other = -1;

        browse_up(&b);
        n = browse_listing(&b, idx, 16);
        if (n == 3)
            other = (idx[0] == warez) ? idx[1] : idx[0];

        if (other >= 0) {
            CHECK(filelist_publish_partial(&ix, "/DL/warez/", 0, xml, bz2,
                                           "MYCID") == 2, "publish failed");
            CHECK(browse_merge_bz2(&b, bz2, other, "/DL/mods/") == -1,
                  "a listing of warez was filed under mods");
            CHECK(browse_needs_fetch(&b, other),
                  "the wrongly-answered directory was marked loaded");
        }
    }

    browse_free(&b);
    share_free(&ix);
}

/* A list that is not compressed.
 *
 * files.xml.bz2 is bzip2 by name. A partial list is not necessarily:
 * AirDC++ answers `CGET list /` with plain XML, and the first version of
 * this client reported "read error in the file list" about perfectly good
 * XML. Both forms have to load, and the only way to be sure is to write
 * one of each and read them back. */
static void test_plain_xml_list(void)
{
    browse_list b;
    char path[384];
    FILE *f;
    long idx[8], n;

    static const char *const PLAIN =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<FileListing Version=\"1\" CID=\"THEIRS\" Base=\"/Share/\" "
        "Generator=\"AirDC++ 4.x\">\r\n"
        "\t<Directory Name=\"warez\" Incomplete=\"1\"/>\r\n"
        "\t<File Name=\"NOTE.TXT\" Size=\"12\" "
        "TTH=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"/>\r\n"
        "</FileListing>\r\n";

    path_of(path, sizeof path, "plain.xml");
    f = fopen(path, "wb");
    CHECK(f != NULL, "cannot write the fixture");
    if (!f)
        return;
    fputs(PLAIN, f);
    fclose(f);

    browse_init(&b);
    n = browse_load_bz2(&b, path, "AirPeer");
    CHECK(n == 2, "an uncompressed list gave %ld entries, expected 2: %s",
          n, browse_error(&b));
    CHECK(!b.whole, "a list with an incomplete directory read as whole");
    CHECK_STR(b.base, "/Share/");

    n = browse_listing(&b, idx, 8);
    CHECK(n == 2, "the listing holds %ld entries", n);
    if (n == 2) {
        CHECK_STR(browse_name(&b, idx[0]), "warez");
        CHECK(browse_needs_fetch(&b, idx[0]),
              "the incomplete directory does not need fetching");
        CHECK_STR(browse_name(&b, idx[1]), "NOTE.TXT");
    }

    /* And merging an uncompressed listing into it. */
    if (n == 2) {
        char sub[384];

        path_of(sub, sizeof sub, "plain2.xml");
        f = fopen(sub, "wb");
        if (f) {
            fputs("<FileListing Version=\"1\" Base=\"/Share/warez/\">\r\n"
                  "\t<File Name=\"DOOM.ZIP\" Size=\"99\" "
                  /* 38 B's and an A: a 39-character base32 TTH carries
                     195 bits for a 192-bit hash, so the last character's
                     low three bits must be zero. An all-B hash is not
                     valid base32 and the decoder rightly refuses it --
                     which, in the first version of this fixture, looked
                     like the merge losing the file. */
                  "TTH=\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBA\"/>\r\n"
                  "</FileListing>\r\n", f);
            fclose(f);

            CHECK(browse_merge_bz2(&b, sub, idx[0], "/Share/warez/") == 1,
                  "merging an uncompressed listing failed: %s",
                  browse_error(&b));
            browse_enter(&b, idx[0]);
            CHECK(browse_listing(&b, idx, 8) == 1,
                  "the merged directory does not hold its file");
            remove(sub);
        }
    }

    /* Something that is neither bzip2 nor XML is an error, not a crash or
       an empty tree that looks like an empty share. */
    {
        browse_list junk;
        char jp[384];

        path_of(jp, sizeof jp, "junk.bin");
        f = fopen(jp, "wb");
        if (f) {
            fputs("\x01\x02 this is not a file list at all", f);
            fclose(f);
            browse_init(&junk);
            CHECK(browse_load_bz2(&junk, jp, "Nobody") <= 0,
                  "rubbish parsed as a file list");
            browse_free(&junk);
            remove(jp);
        }
    }

    browse_free(&b);
    remove(path);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    int oracle;
    char cmd[512];

    printf("DOSy++ browse tests (bzip2 / file-list parser / navigation)\n");

    snprintf(g_dir, sizeof g_dir, "%s/dosypp_browse_test",
             tmp && tmp[0] ? tmp : "/tmp");
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0) {
        printf("FAILED: cannot create %s\n", g_dir);
        return 1;
    }

    oracle = have_bzip2();

    test_bzip2(oracle);
    test_unescape();
    test_parse();
    test_parse_hostile();
    test_round_trip();
    test_partial_round_trip();
    test_plain_xml_list();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)system(cmd);

    if (g_fail)
        printf("FAILED: %d of %d checks\n", g_fail, g_run);
    else
        printf("ok: %d checks, 0 failed\n", g_run);
    return g_fail ? 1 : 0;
}
