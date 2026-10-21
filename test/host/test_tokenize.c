/* test_tokenize.c - native unit tests for the ADC tokenizer.
 * Built and run by `make test` on the host, never in DOSBox. */
#include <stdio.h>
#include <string.h>

#include "../../src/adc/tokenize.h"

static int g_fail;
static int g_run;

#define CHECK(cond) do {                                                   \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want) do {                                          \
        g_run++;                                                           \
        if (!(got) || strcmp((got), (want)) != 0) {                        \
            printf("FAIL %s:%d  got \"%s\" want \"%s\"\n",                 \
                   __FILE__, __LINE__, (got) ? (got) : "(null)", (want));  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static void test_escape(void)
{
    char buf[64];

    CHECK(adc_escape(buf, sizeof buf, "hello world") == 12);
    CHECK_STR(buf, "hello\\sworld");

    CHECK(adc_escape(buf, sizeof buf, "a\\b") > 0);
    CHECK_STR(buf, "a\\\\b");

    CHECK(adc_escape(buf, sizeof buf, "two\nlines") > 0);
    CHECK_STR(buf, "two\\nlines");

    CHECK(adc_escape(buf, sizeof buf, "") == 0);
    CHECK_STR(buf, "");

    /* Must refuse rather than truncate: a truncated filename is worse than
       a failed send. */
    CHECK(adc_escape(buf, 4, "aaaa") == -1);
    CHECK(adc_escape(buf, 2, " ") == -1);
}

static void test_unescape(void)
{
    char buf[64];

    CHECK(adc_unescape(buf, sizeof buf, "hello\\sworld") == 11);
    CHECK_STR(buf, "hello world");

    CHECK(adc_unescape(buf, sizeof buf, "a\\\\b") == 3);
    CHECK_STR(buf, "a\\b");

    CHECK(adc_unescape(buf, sizeof buf, "x\\ny") == 3);
    CHECK_STR(buf, "x\ny");

    /* Undefined escape: keep the backslash rather than lose a byte. */
    CHECK(adc_unescape(buf, sizeof buf, "a\\qb") == 4);
    CHECK_STR(buf, "a\\qb");

    /* Trailing lone backslash must not read past the NUL. */
    CHECK(adc_unescape(buf, sizeof buf, "end\\") == 4);
    CHECK_STR(buf, "end\\");
}

static void test_roundtrip(void)
{
    const char *cases[] = {
        "plain",
        "with space",
        "back\\slash",
        "new\nline",
        "all \\ three\n at once",
        "",
    };
    char esc[256], back[256];
    size_t i;

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        CHECK(adc_escape(esc, sizeof esc, cases[i]) >= 0);
        CHECK(adc_unescape(back, sizeof back, esc) >= 0);
        CHECK_STR(back, cases[i]);
        /* An escaped parameter must never contain a raw separator. */
        CHECK(strchr(esc, ' ') == NULL);
        CHECK(strchr(esc, '\n') == NULL);
    }
}

static void test_parse(void)
{
    adc_msg m;

    CHECK(adc_parse(&m, "ISUP ADBASE ADTIGR") == 0);
    CHECK(m.type == 'I');
    CHECK_STR(m.cmd, "SUP");
    CHECK(m.nparam == 2);
    CHECK_STR(m.param[0], "ADBASE");
    CHECK_STR(m.param[1], "ADTIGR");
    CHECK(adc_hdr_params(m.type) == 0);

    CHECK(adc_parse(&m, "ISID AAAB") == 0);
    CHECK_STR(m.param[0], "AAAB");

    /* No parameters at all. */
    CHECK(adc_parse(&m, "HDSC") == 0);
    CHECK(m.nparam == 0);
    CHECK_STR(m.cmd, "DSC");

    /* Empty line is a keepalive, not an error. */
    CHECK(adc_parse(&m, "") == 0);
    CHECK(m.type == 0);
    CHECK(m.nparam == 0);

    /* Malformed. */
    CHECK(adc_parse(&m, "XSUP") == -1);      /* bad type */
    CHECK(adc_parse(&m, "Isu") == -1);       /* too short */
    CHECK(adc_parse(&m, "Isup x") == -1);    /* lowercase command */
    CHECK(adc_parse(&m, "ISUPX") == -1);     /* no separator after command */
}

static void test_fields(void)
{
    adc_msg m;

    CHECK(adc_parse(&m,
        "BINF AAAB IDQWERTY NIsome\\snick SS1234 SL2 VEDOSy++\\s0.1") == 0);
    CHECK(m.type == 'B');
    CHECK(adc_hdr_params('B') == 1);
    CHECK_STR(m.param[0], "AAAB");            /* SID, positional */
    CHECK_STR(adc_field(&m, "NI"), "some nick");
    CHECK_STR(adc_field(&m, "ID"), "QWERTY");
    CHECK_STR(adc_field(&m, "SS"), "1234");
    CHECK_STR(adc_field(&m, "VE"), "DOSy++ 0.1");
    CHECK(adc_field(&m, "ZZ") == NULL);

    /* The SID must not be mistaken for a field, even when it looks like one.
       SID "IDAB" would otherwise answer a lookup for "ID". */
    CHECK(adc_parse(&m, "BINF IDAB NIbob") == 0);
    CHECK(adc_field(&m, "ID") == NULL);
    CHECK_STR(adc_field(&m, "NI"), "bob");

    /* D has two header params: from and to. */
    CHECK(adc_parse(&m, "DMSG AAAB AAAC hello PMAAAB") == 0);
    CHECK(adc_hdr_params('D') == 2);
    CHECK_STR(m.param[0], "AAAB");
    CHECK_STR(m.param[1], "AAAC");
    CHECK_STR(m.param[2], "hello");
    CHECK_STR(adc_field(&m, "PM"), "AAAB");

    /* A chat line is a positional param and may contain anything. */
    CHECK(adc_parse(&m, "BMSG AAAB warez\\sor\\sdeath") == 0);
    CHECK_STR(m.param[1], "warez or death");
}

static void test_linebuf(void)
{
    linebuf lb;
    char out[ADC_LINE_MAX];
    char big[ADC_LINE_MAX + 200];
    size_t i;

    linebuf_init(&lb);
    CHECK(linebuf_next(&lb, out) == 0);

    /* Two messages in one read. */
    CHECK(linebuf_feed(&lb, "ISUP A\nISID B\n", 14) == 0);
    CHECK(linebuf_next(&lb, out) == 1);
    CHECK_STR(out, "ISUP A");
    CHECK(linebuf_next(&lb, out) == 1);
    CHECK_STR(out, "ISID B");
    CHECK(linebuf_next(&lb, out) == 0);

    /* One message split across three reads -- the normal case on a 14.4k
       link, and the thing a naive recv-per-message would get wrong. */
    linebuf_init(&lb);
    CHECK(linebuf_feed(&lb, "IIN", 3) == 0);
    CHECK(linebuf_next(&lb, out) == 0);
    CHECK(linebuf_feed(&lb, "F NIhub", 7) == 0);
    CHECK(linebuf_next(&lb, out) == 0);
    CHECK(linebuf_feed(&lb, "name\n", 5) == 0);
    CHECK(linebuf_next(&lb, out) == 1);
    CHECK_STR(out, "IINF NIhubname");

    /* Keepalive: a bare newline yields an empty line, not nothing. */
    linebuf_init(&lb);
    CHECK(linebuf_feed(&lb, "\n", 1) == 0);
    CHECK(linebuf_next(&lb, out) == 1);
    CHECK_STR(out, "");

    /* Over-long line is rejected, then the stream resynchronises at the next
       newline so one bad message does not kill the session. */
    linebuf_init(&lb);
    for (i = 0; i < sizeof big - 1; i++)
        big[i] = 'x';
    big[sizeof big - 1] = '\0';
    CHECK(linebuf_feed(&lb, big, strlen(big)) == -1);
    CHECK(linebuf_feed(&lb, "junk\nISID B\n", 12) == 0);
    CHECK(linebuf_next(&lb, out) == 1);
    CHECK_STR(out, "ISID B");
}

/* 64-bit sizes on a 32-bit target.
 *
 * ADC sizes are 64-bit and "%lu" is 32 bits on DOS, so a share over 4GB
 * printed the ordinary way advertises the remainder modulo 4GB -- a 5GB
 * share telling a hub it has 700MB. And a size read from the network has
 * to saturate rather than wrap, because a wrapped one would be acted on.
 */
static void test_u64_text(void)
{
    char out[24];

    adc_u64_dec(0, out, sizeof out);
    CHECK_STR(out, "0");
    adc_u64_dec(1, out, sizeof out);
    CHECK_STR(out, "1");
    adc_u64_dec(100000000UL, out, sizeof out);
    CHECK_STR(out, "100000000");

    /* 4GB and just past it: where (unsigned long) starts lying. */
    adc_u64_dec((tiger_u64)4294967295UL, out, sizeof out);
    CHECK_STR(out, "4294967295");
    adc_u64_dec((tiger_u64)4294967295UL + 1, out, sizeof out);
    CHECK_STR(out, "4294967296");

    /* A plausible big share: 5GB. */
    adc_u64_dec((tiger_u64)5 * 1024 * 1024 * 1024, out, sizeof out);
    CHECK_STR(out, "5368709120");

    /* And a 10TB one, which is the figure the hub-list parser once
       truncated to 10GB. */
    adc_u64_dec((tiger_u64)10 * 1024 * 1024 * 1024 * 1024, out, sizeof out);
    CHECK_STR(out, "10995116277760");

    /* A buffer too small truncates rather than overruns. */
    adc_u64_dec(123456789UL, out, 5);
    CHECK(strlen(out) == 4);

    /* Parsing: ordinary, then absurd. */
    CHECK(adc_u64_parse("0") == 0);
    CHECK(adc_u64_parse("100000000") == 100000000UL);
    CHECK(adc_u64_parse("5368709120") == (tiger_u64)5 * 1024 * 1024 * 1024);
    CHECK(adc_u64_parse(NULL) == 0);
    CHECK(adc_u64_parse("12x34") == 12);

    /* Thirty digits: saturated, not wrapped. A wrap could read as a small
       file and be queued. */
    {
        tiger_u64 huge = adc_u64_parse("999999999999999999999999999999");

        CHECK(huge > (tiger_u64)1024 * 1024 * 1024 * 1024);
    }

    /* Round trip, at the sizes that matter. */
    {
        static const tiger_u64 SIZES[] = {
            1UL, 1024UL, 100000000UL, 2147483647UL, 4294967296UL
        };
        size_t k;

        for (k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
            adc_u64_dec(SIZES[k], out, sizeof out);
            CHECK(adc_u64_parse(out) == SIZES[k]);
        }
    }
}

int main(void)
{
    test_escape();
    test_unescape();
    test_roundtrip();
    test_parse();
    test_fields();
    test_linebuf();
    test_u64_text();

    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
