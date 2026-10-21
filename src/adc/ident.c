/* ident.c - SIDs, PID/CID derivation, and the PAS response. */
#include "ident.h"

#include <string.h>
#include <time.h>

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

void adc_sid_encode(adc_sid sid, char *out)
{
    int i;

    /* Four base32 characters carry 20 bits, most significant first. */
    for (i = 3; i >= 0; i--) {
        out[i] = B32[sid & 0x1F];
        sid >>= 5;
    }
    out[4] = '\0';
}

int adc_sid_decode(const char *text, adc_sid *out)
{
    adc_sid v = 0;
    int i;

    if (!text)
        return -1;

    for (i = 0; i < ADC_SID_CHARS; i++) {
        char c = text[i];
        int d;

        if (c >= 'A' && c <= 'Z')
            d = c - 'A';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a';
        else if (c >= '2' && c <= '7')
            d = c - '2' + 26;
        else
            return -1;

        v = (v << 5) | (adc_sid)d;
    }

    /* Reject trailing junk: a SID is exactly four characters. */
    if (text[ADC_SID_CHARS] != '\0')
        return -1;

    *out = v;
    return 0;
}

void adc_identity_derive(adc_identity *id)
{
    unsigned char digest[TIGER_DIGEST_LEN];

    base32_encode(id->pid_b32, sizeof id->pid_b32, id->pid, ADC_PID_BYTES);

    tiger(id->pid, ADC_PID_BYTES, digest);
    base32_encode(id->cid_b32, sizeof id->cid_b32, digest, TIGER_DIGEST_LEN);
}

int adc_identity_from_b32(adc_identity *id, const char *pid_b32)
{
    unsigned char buf[ADC_PID_BYTES + 4];
    int n;

    memset(id, 0, sizeof *id);

    n = base32_decode(buf, sizeof buf, pid_b32);
    if (n != ADC_PID_BYTES)
        return -1;

    memcpy(id->pid, buf, ADC_PID_BYTES);
    adc_identity_derive(id);
    return 0;
}

void adc_identity_generate(adc_identity *id, unsigned long salt)
{
    /* Stir whatever varies into Tiger and take the digest as the PID. Tiger
       gives 24 bytes, which is exactly the PID length -- convenient, and it
       means a weak seed is at least spread across all 24 bytes rather than
       leaving most of them predictable. */
    struct {
        unsigned long t;
        unsigned long c;
        unsigned long salt;
        unsigned long addr;
    } seed;
    unsigned char digest[TIGER_DIGEST_LEN];

    memset(id, 0, sizeof *id);

    seed.t = (unsigned long)time(NULL);
    seed.c = (unsigned long)clock();
    seed.salt = salt;
    /* Stack address: varies a little with DPMI allocation. Not much, but
       it costs nothing. */
    seed.addr = (unsigned long)(size_t)&seed;

    tiger(&seed, sizeof seed, digest);
    memcpy(id->pid, digest, ADC_PID_BYTES);

    adc_identity_derive(id);
}

int adc_password_response(const char *password, const char *random_b32,
                          char *out, size_t outsz)
{
    /* Hubs send 24 bytes or more; allow generous headroom and refuse rather
       than truncate, since a truncated challenge gives a wrong answer that
       looks like a wrong password. */
    unsigned char random[128];
    unsigned char digest[TIGER_DIGEST_LEN];
    tiger_ctx t;
    int n;

    if (outsz < BASE32_TIGER_LEN + 1)
        return -1;

    n = base32_decode(random, sizeof random, random_b32);
    if (n <= 0)
        return -1;

    tiger_init(&t);
    tiger_update(&t, password, strlen(password));
    tiger_update(&t, random, (size_t)n);
    tiger_final(&t, digest);

    base32_encode(out, outsz, digest, TIGER_DIGEST_LEN);
    return 0;
}
