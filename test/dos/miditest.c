/* miditest.c - drive a Roland MPU-401 and make a sound somebody can measure.
 *
 * An MPU-401 is a serial port, not an instrument. This program does the DOS
 * half of premium game audio -- find the interface, put it in UART mode and
 * write MIDI notes at it -- and leaves the other half, the synthesiser, to
 * whatever is listening. Under DOSBox-X that is FluidSynth with a
 * SoundFont; on a 1994 desk it was an MT-32 or an SC-55 on the end of a
 * cable.
 *
 * Why a program rather than a game: the rig has to know exactly what should
 * be heard and when. This plays a known pattern --
 *
 *     chord ... silence ... chord
 *
 * -- so test/dos/run-dosbox-midi.sh can measure the captured audio in
 * windows and check that the loud parts are loud, the gap is quiet, and the
 * whole thing is not just noise. "Some energy came out" would pass with a
 * stuck DAC or a startup beep.
 *
 * The MPU-401 interface, in full:
 *
 *   0x330  data     read: data/ACK from the card, write: data to the card
 *   0x331  command  read: status, write: a command
 *
 * Status bit 7 (0x80) set   = no data waiting to be read
 * Status bit 6 (0x40) set   = not ready to be written to
 *
 * Intelligent mode is the mode DOSBox-X emulates by default, and a reset
 * (0xFF) is acknowledged with 0xFE. Then 0x3F asks for UART mode, after
 * which the card is a plain MIDI pipe and every byte written to 0x330 goes
 * down the wire. Games did exactly this; so does this.
 *
 *   miditest            play the pattern
 *   miditest --probe    report whether an MPU-401 answered, and exit
 *
 * Built by the DOS build; run by make dos-midi-check.
 */
#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MPU_DATA 0x330
#define MPU_CMD  0x331

#define ST_NO_DATA   0x80       /* nothing to read */
#define ST_NOT_READY 0x40       /* cannot write yet */

#define MPU_ACK      0xFE
#define CMD_RESET    0xFF
#define CMD_UART     0x3F

/* Spin limits rather than timeouts in seconds: this runs on anything from a
   386 to an emulator pretending to be one, and a wall-clock timeout needs a
   clock that works on all of them. These are generous -- a real card answers
   in microseconds. */
#define SPINS 60000L

static int wait_writable(void)
{
    long spins = 0;

    while ((inportb(MPU_CMD) & ST_NOT_READY) && ++spins < SPINS)
        ;
    return spins < SPINS;
}

static int wait_readable(void)
{
    long spins = 0;

    while ((inportb(MPU_CMD) & ST_NO_DATA) && ++spins < SPINS)
        ;
    return spins < SPINS;
}

/* A command goes to 0x331 and is acknowledged on 0x330. The ACK may be
   preceded by leftovers from whatever ran before us, so a few bytes are
   read before giving up on it. */
static int mpu_command(unsigned char cmd)
{
    int tries;

    if (!wait_writable())
        return 0;
    outportb(MPU_CMD, cmd);

    for (tries = 0; tries < 8; tries++) {
        if (!wait_readable())
            return 0;
        if (inportb(MPU_DATA) == MPU_ACK)
            return 1;
    }
    return 0;
}

static int mpu_byte(unsigned char b)
{
    if (!wait_writable())
        return 0;
    outportb(MPU_DATA, b);
    return 1;
}

static int mpu_send(const unsigned char *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (!mpu_byte(bytes[i]))
            return 0;
    return 1;
}

/* clock() is the only portable timer here and on DOS it ticks every 55ms,
   so a wait of less than that is not expressible. Everything this program
   times is in hundreds of milliseconds for that reason. */
static void wait_ms(unsigned long ms)
{
    clock_t start = clock();
    clock_t ticks = (clock_t)((ms * CLOCKS_PER_SEC) / 1000UL);

    if (ticks < 1)
        ticks = 1;
    while ((clock_t)(clock() - start) < ticks)
        ;
}

static void note_on(unsigned char note, unsigned char vel)
{
    unsigned char msg[3];

    msg[0] = 0x90;              /* note on, channel 1 */
    msg[1] = note;
    msg[2] = vel;
    mpu_send(msg, 3);
}

static void note_off(unsigned char note)
{
    unsigned char msg[3];

    msg[0] = 0x80;              /* note off, channel 1 */
    msg[1] = note;
    msg[2] = 0x40;
    mpu_send(msg, 3);
}

int main(int argc, char **argv)
{
    /* An organ: it sustains at a steady level for as long as the key is
       held, which is what makes a window of captured audio measurable. A
       piano's decay would make "quiet" and "the end of a note" the same
       measurement. */
    static const unsigned char PROGRAM = 19;   /* GM 20, church organ */
    static const unsigned char CHORD[3] = { 60, 64, 67 };   /* C major */
    int probe_only = (argc > 1 && strcmp(argv[1], "--probe") == 0);
    int i, round;

    printf("MIDI: MPU-401 at %03Xh, resetting\n", MPU_DATA);
    fflush(stdout);

    if (!mpu_command(CMD_RESET)) {
        printf("MIDI: no MPU-401 answered a reset at %03Xh\n", MPU_DATA);
        printf("MIDI: nothing to play through\n");
        return 1;
    }
    printf("MIDI: reset acknowledged\n");

    if (!mpu_command(CMD_UART)) {
        /* Some implementations do not acknowledge the switch to UART mode,
           so this is reported and not treated as fatal: the writes that
           follow will show whether it took. */
        printf("MIDI: UART mode was not acknowledged, carrying on\n");
    } else {
        printf("MIDI: UART mode\n");
    }
    fflush(stdout);

    if (probe_only) {
        printf("MIDI: probe only, nothing played\n");
        return 0;
    }

    /* Channel volume and the instrument, before anything sounds. */
    {
        unsigned char setup[6];

        setup[0] = 0xC0; setup[1] = PROGRAM;            /* program change */
        setup[2] = 0xB0; setup[3] = 0x07; setup[4] = 127;  /* volume */
        mpu_send(setup, 5);
    }

    /* The pattern the rig measures: loud, quiet, loud. Two rounds of it,
       because one of anything is hard to tell from an accident. */
    for (round = 0; round < 2; round++) {
        printf("MIDI: chord %d on\n", round + 1);
        fflush(stdout);
        for (i = 0; i < 3; i++)
            note_on(CHORD[i], 100);
        wait_ms(1500);

        for (i = 0; i < 3; i++)
            note_off(CHORD[i]);
        printf("MIDI: chord %d off\n", round + 1);
        fflush(stdout);
        wait_ms(1500);
    }

    /* All notes off, on every channel: a sustained note left ringing would
       be heard in the silence the rig expects at the end. */
    for (i = 0; i < 16; i++) {
        unsigned char msg[3];

        msg[0] = (unsigned char)(0xB0 | i);
        msg[1] = 123;           /* all notes off */
        msg[2] = 0;
        mpu_send(msg, 3);
    }

    printf("MIDI: done\n");
    fflush(stdout);
    return 0;
}
