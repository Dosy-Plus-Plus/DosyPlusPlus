/* ident.h - ADC identity: SIDs, PID/CID, and the password response.
 *
 * Three small things that each cause a login to fail in a way the hub does
 * not explain well:
 *
 *   SID  20 bits, carried as exactly 4 base32 characters. Assigned by the
 *        hub in ISID. Appears as the first parameter of every B/D/E/F
 *        message, so it is not a field and must never be read as one.
 *
 *   CID  = base32(tiger(PID)). The hub recomputes this from the PD field we
 *        send and rejects us if it disagrees (ISTA 27 "invalid PID", or 24
 *        "CID taken"). PID is 24 random bytes, generated once and kept.
 *
 *   PAS  = base32(tiger(utf8(password) || decoded_random)). The random comes
 *        from IGPA, base32-encoded, and must be DECODED before hashing. Note
 *        the PID is NOT involved -- a plausible-looking mistake that yields
 *        ISTA 23 "invalid password" with a correct password.
 *
 * No DOS dependency; unit-tested natively.
 */
#ifndef DOSYPP_IDENT_H
#define DOSYPP_IDENT_H

#include <stddef.h>

#include "../hash/base32.h"
#include "../hash/tiger.h"

#define ADC_PID_BYTES  24
#define ADC_SID_CHARS  4
#define ADC_CID_CHARS  BASE32_TIGER_LEN        /* 39 */

/* A SID as the 20-bit value the spec defines. 0 is not a valid user SID
   (the hub itself uses it), so it doubles as "none". */
typedef unsigned long adc_sid;

#define ADC_SID_NONE ((adc_sid)0)
#define ADC_SID_MAX  ((adc_sid)0xFFFFF)        /* 20 bits */

/* Encode a SID as 4 base32 characters. out needs 5 bytes. */
void adc_sid_encode(adc_sid sid, char *out);

/* Decode exactly 4 base32 characters. Returns 0 on success, -1 if the text
   is not 4 valid characters. */
int adc_sid_decode(const char *text, adc_sid *out);

typedef struct {
    unsigned char pid[ADC_PID_BYTES];
    char pid_b32[ADC_CID_CHARS + 1];   /* what goes in the PD field */
    char cid_b32[ADC_CID_CHARS + 1];   /* what goes in the ID field */
} adc_identity;

/* Derive the base32 PID and CID from id->pid. Call after setting pid. */
void adc_identity_derive(adc_identity *id);

/* Load an identity from a stored base32 PID. Returns 0 on success, -1 if
   the string does not decode to exactly ADC_PID_BYTES bytes. */
int adc_identity_from_b32(adc_identity *id, const char *pid_b32);

/* Generate a fresh identity.
 *
 * DOS has no system entropy source, so this mixes the clock, the tick
 * counter and a caller-supplied salt through Tiger. That is adequate for an
 * identity but is NOT a secure random: see the entropy item in TODO.md,
 * which has to be solved properly before native TLS. */
void adc_identity_generate(adc_identity *id, unsigned long salt);

/* Build the PAS response for a given password and the base32 random from
   IGPA. Writes up to ADC_CID_CHARS+1 bytes to out.
   Returns 0 on success, -1 if the random does not decode. */
int adc_password_response(const char *password, const char *random_b32,
                          char *out, size_t outsz);

#endif /* DOSYPP_IDENT_H */
