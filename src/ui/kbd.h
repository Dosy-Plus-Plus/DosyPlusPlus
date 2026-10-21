/* kbd.h - non-blocking keyboard.
 *
 * Returns 0 when nothing is waiting: the main loop must keep servicing the
 * network while the user thinks. Extended keys (arrows, function keys) come
 * back as KEY_* values above 0xFF so they cannot collide with characters.
 *
 * On DOS this installs a custom int 9h handler, because the BIOS is not good
 * enough for this program:
 *
 *   - The BIOS buffer holds 16 keys and silently drops the rest. During a
 *     transfer, with the main loop busy, that is easy to overflow.
 *   - There are no key-up events, so "is shift held" is unanswerable.
 *   - The typematic rate is whatever the user's BIOS set, which makes a
 *     lightbar feel sluggish. With the controller in our hands we ask for
 *     the fast rate and restore the default on exit.
 *
 * The risk is real: an int 9h handler that is not restored leaves a machine
 * needing a reboot. So the vector is restored from atexit AND from handlers
 * for the fatal signals, and `--bios-kbd` falls back to int 16h if the ISR
 * misbehaves on hardware we have not seen.
 */
#ifndef DOSYPP_KBD_H
#define DOSYPP_KBD_H

#define KEY_NONE      0

#define KEY_ENTER     13
#define KEY_ESC       27
#define KEY_TAB       9
#define KEY_BACKSPACE 8

#define KEY_UP        0x100
#define KEY_DOWN      0x101
#define KEY_LEFT      0x102
#define KEY_RIGHT     0x103
#define KEY_HOME      0x104
#define KEY_END       0x105
#define KEY_PGUP      0x106
#define KEY_PGDN      0x107
#define KEY_DEL       0x108
#define KEY_INS       0x109

#define KEY_F1        0x110
#define KEY_F2        0x111
#define KEY_F3        0x112
#define KEY_F4        0x113
#define KEY_F5        0x114
#define KEY_F6        0x115
#define KEY_F7        0x116
#define KEY_F8        0x117
#define KEY_F9        0x118
#define KEY_F10       0x119

/* Modifier bits from kbd_mods(). */
#define KBD_SHIFT 0x01
#define KBD_CTRL  0x02
#define KBD_ALT   0x04

/* Pass non-zero to force the BIOS path on DOS (the --bios-kbd escape
   hatch). Ignored on the host. */
void kbd_open(void);
void kbd_open_bios(void);
void kbd_close(void);

/* Next key, or KEY_NONE. */
int kbd_get(void);

/* Modifiers currently held. Always 0 on the BIOS path and on the host,
   where key-up is not observable. */
unsigned kbd_mods(void);

/* Non-zero if the ISR is in use rather than the BIOS. For the info door,
   so a support question can be answered. */
int kbd_have_isr(void);

/* Keys delivered and keys dropped because the ring filled. For diagnosing
   "the keyboard does nothing", which is otherwise unfalsifiable: it
   distinguishes "the ISR never fired" from "the UI ignored the key". */
void kbd_stats(unsigned long *got, unsigned long *lost);

/* Inject a scancode as though it had been typed, using the keyboard
 * controller's "write to output buffer" command (0x64 <- 0xD2). The
 * controller then raises IRQ1, so this exercises the ENTIRE real path:
 * controller, interrupt, our ISR, decode, ring buffer.
 *
 * It exists because a keyboard driver is otherwise untestable without
 * hands: DOSBox-X's AUTOTYPE is synchronous, so its keystrokes land at the
 * command prompt before the program under test has started.
 *
 * Returns 0 if the injection was accepted, -1 where unsupported (the host,
 * or the BIOS fallback). The physical keypress is the only simulated part. */
int kbd_inject(unsigned char scancode);

/* --- scancode decoding, exposed for tests -------------------------------
 *
 * Decode one scancode-set-1 make code. `ext` is non-zero when a 0xE0
 * prefix preceded it; `mods` is a KBD_* bitmask. Returns a key, or
 * KEY_NONE for a release, a modifier, or an unmapped code.
 *
 * Portable on purpose: these tables are exactly the kind of data where one
 * wrong entry goes unnoticed until somebody types that character. */
int kbd_decode(unsigned char sc, int ext, unsigned mods);
int kbd_special_key(unsigned char sc);

#endif /* DOSYPP_KBD_H */
