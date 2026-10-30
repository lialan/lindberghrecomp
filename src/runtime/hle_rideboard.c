/*
 * hle_rideboard.c - the ride board of a large attraction cabinet.
 *
 * The House of the Dead 4 Special (and Let's Go Jungle Special) seat their
 * players in a cabinet that turns, shakes and blows air, run by a board on the
 * base board's first serial port. The game opens /dev/ttyS0 and, every frame,
 * writes a 7-byte request and reads back a 22-byte status; until a status
 * arrives it stays on a black screen, waiting for the seat to come up.
 *
 * There is no seat here, so this is the board's side of the conversation for
 * a cabinet that is healthy, has its motion switched off and its seats ready,
 * and whose operator does what the start up asks. The game then runs as it
 * would with the motion disabled by the operator.
 *
 * The packets, as documented by bobbydilley's rideboard-emulator:
 *   request  C0 command seat blowers lamp lamps xor(1..5)
 *   status   C0 00 command' seat' buttons sensors position 00 ... xor(1..20)
 * command' and seat' acknowledge the request; the rest is the switches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#define RIDE_FD 0x7D00          /* a descriptor nothing else hands out */

static const char *g_dev;       /* the device path that is the board, or NULL */
static int g_open;
static unsigned char g_req[7];
static unsigned g_req_len;
static unsigned char g_status[22];
static int g_pending;           /* a status is waiting to be read */
static unsigned char g_command, g_seat;
static unsigned g_waiting;      /* requests spent in the start up's operator step */

void rideboard_enable(const char *dev) { g_dev = dev; }

int rideboard_open(const char *path)
{
    if (!g_dev || !path || strcmp(path, g_dev) != 0) return -1;
    g_open = 1; g_req_len = 0; g_pending = 0;
    fprintf(stderr, "[ride] %s is the ride board: motion off, seats ready\n", path);
    return RIDE_FD;
}

int rideboard_is(int fd) { return g_open && fd == RIDE_FD; }

/* What the board says to a command. Its reply is the state the ride is in:
 * while a command is being carried out that is the command itself, and when
 * it is done, the state it led to - which is what the game waits for before
 * its next step. Each move takes a quarter of a second, as a seat would.
 *
 * The game's two routines (start up, then running) both use 06: in the start
 * up it homes the seat and finishes with 1A, "initialised"; afterwards it is a
 * step of the game's own ride sequence. */
static const unsigned char k_done[256] = {
    [0x02] = 0x03,      /* initial check */
    [0x04] = 0x05,      /* reset */
    [0x10] = 0x14,      /* stand by - an echo of 10 reads as "ride stopped" */
    [0x14] = 0x15,
    [0x15] = 0x05,      /* return home: the operator's RESET, the move, then "reset done" */
    [0x12] = 0x14,
    [0x17] = 0x12,
    [0x0F] = 0x0C,
    [0x19] = 0x06,
    [0x06] = 0x08, [0x07] = 0x08,
    [0x08] = 0x0A,
};
static unsigned char g_last_cmd;
static unsigned g_moving;       /* requests since the current command arrived */
static int g_running;           /* the start up is over (the game has sent 10) */

static unsigned char ack_command(unsigned char cmd)
{
    g_moving = cmd == g_last_cmd ? g_moving + 1 : 0;
    g_last_cmd = cmd;
    if (cmd == 0x10) g_running = 1;
    if (cmd == 0x03) return g_command;      /* sensor clear: no new state */
    unsigned char done = cmd == 0x06 && !g_running ? 0x1A : k_done[cmd];
    if (!done) return cmd;
    /* Stand by is answered at once: in the game's running states an echo of
     * 10 means the ride stopped itself, which raises a safety stop. */
    if (cmd == 0x10) return done;
    return g_moving < 15 ? cmd : done;
}

/* The seat's turn commands, 0x01 and 0x11..0x61, are acknowledged as done. */
static unsigned char ack_seat(unsigned char seat)
{
    if (seat == 0x01) return 1;
    if ((seat & 0x0F) == 0x01 && seat >= 0x11 && seat <= 0x61) return (unsigned char)(((seat >> 4) - 1) * 2 + 1);
    return g_seat;
}

static void answer(void)
{
    unsigned char x = 0;
    for (int i = 1; i < 6; i++) x ^= g_req[i];
    if (x != g_req[6] && getenv("LINDBERGH_RIDE"))
        fprintf(stderr, "[ride] request checksum %02X, expected %02X\n", g_req[6], x);
    g_command = ack_command(g_req[1]);
    g_seat = ack_seat(g_req[2]);

    memset(g_status, 0, sizeof g_status);
    g_status[0] = 0xC0;
    g_status[2] = g_command;
    g_status[3] = g_seat;
    /* The start up asks the operator to check the ride and press INITIALIZE
     * (command 03, "ride initial setup"); the switch is pressed for a moment
     * once the game has been waiting half a second. */
    g_waiting = g_req[1] == 0x03 ? g_waiting + 1 : 0;
    if (g_waiting >= 30 && g_waiting < 40)
        g_status[4] = 0x10;     /* INITIALIZE */
    /* Byte 5's sensors read as faults when set - a foot on a floor mat,
     * an armrest up, a belt undone - so a ready cabinet clears them all. */
    g_status[5] = 0x00;
    /* 0A is the belt check before a game: the game watches each belt's
     * sensor go up and come down again as the player buckles in, so the
     * players buckle in a moment after it asks. */
    if (g_req[1] == 0x0A && g_moving >= 10 && g_moving < 20)
        g_status[5] = 0x30;
    g_status[6] = 0x80;         /* the motion select switch at "off" */
    x = 0;
    for (int i = 1; i < 21; i++) x ^= g_status[i];
    g_status[21] = x;
    g_pending = 1;
    if (getenv("LINDBERGH_RIDE")) {
        static unsigned char last[8];
        unsigned char now[8] = { g_req[1], g_req[2], g_req[3], g_req[4], g_req[5],
                                 g_command, g_seat, g_status[4] };
        if (memcmp(now, last, sizeof now)) {
            memcpy(last, now, sizeof now);
            fprintf(stderr, "[ride] %02X %02X %02X %02X %02X -> %02X %02X buttons %02X\n",
                    now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
        }
    }
}

int rideboard_write(const unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (g_req_len == 0 && p[i] != 0xC0) continue;       /* wait for a sync */
        g_req[g_req_len++] = p[i];
        if (g_req_len == sizeof g_req) { answer(); g_req_len = 0; }
    }
    return (int)n;
}

/* The game reads a whole status or counts a miss; it never reads part of one. */
int rideboard_read(unsigned char *p, unsigned n)
{
    if (!g_pending || n < sizeof g_status) return 0;
    memcpy(p, g_status, sizeof g_status);
    g_pending = 0;
    return (int)sizeof g_status;
}

/* tcflush: a half-sent request goes. A status stays - the real board answers
 * after the game has flushed and slept, and so, in effect, does this one. */
void rideboard_flush(void) { g_req_len = 0; }

void rideboard_close(void) { g_open = 0; }

/* The termios calls the game makes on the port. Only the board has a port. */
static void h_tcgetattr(CPU *c)
{
    if (!rideboard_is((int)A32(0))) { RET(-1); return; }
    if (A32(1)) memset(APTR(1), 0, 60);     /* struct termios on i386 Linux */
    RET(0);
}
static void h_tcsetattr(CPU *c) { RET(rideboard_is((int)A32(0)) ? 0 : -1); }
static void h_tcflush(CPU *c)
{
    if (!rideboard_is((int)A32(0))) { RET(-1); return; }
    rideboard_flush();
    RET(0);
}

void hle_register_rideboard(void)
{
    hle_bind("tcgetattr", h_tcgetattr);
    hle_bind("tcsetattr", h_tcsetattr);
    hle_bind("tcflush", h_tcflush);
}
