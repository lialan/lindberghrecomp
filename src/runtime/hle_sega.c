/*
 * hle_sega.c - the cabinet's own library, answered from here.
 *
 * A Lindbergh game does not talk to its hardware directly. It calls SEGA's
 * amLib: a C API, statically linked into this binary, that reaches a base
 * board over a serial link - the JVS I/O board with the coin slots, buttons
 * and guns on it, the EEPROM holding the bookkeeping, the security dongle.
 * None of that exists here, so every one of those calls fails and the game
 * stops on "Error 11 - JVS I/O board is not connected to main board" before
 * it ever reaches its attract mode.
 *
 * These are plain C symbols, so they override by name the same way the
 * XF86VidMode entry points do. What is answered here is only what the game
 * actually asks on the way to attract mode; the game narrates the rest
 * through _sDebug::putConsole (LINDBERGH_CONSOLE=1), which is how each of
 * these was found rather than guessed.
 */

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <zlib.h>

#include "lindbergh_rt.h"
#if !defined(_WIN32)
#include "host_settings.h"
#ifndef _WIN32
#include "stereo.h"
#endif
#endif

/* The library's own success code. Every am* function returns 0 for success
 * and a negative value for a failure the game then reports. */
#define AM_OK 0

/* The JVS packet the game handed to the transport.
 *
 * Request layout, read off amJvspClearPacket and amJvsSendRequest: byte 0 is
 * the node the packet is addressed to, byte 1 the length of the frame, and
 * bytes 2.. the frame itself. The acknowledge comes back in the same object
 * at byte 0x101 (length) and 0x102.. (frame).
 *
 * LINDBERGH_JVS=1 prints what is asked, which is the only honest way to find
 * out what this board has to be able to answer. */
static void jvs_dump(const char *tag, uint32_t pkt, unsigned off, unsigned len)
{
    const unsigned char *p = (const unsigned char *)GP(pkt);
    char line[512];
    int at = snprintf(line, sizeof line, "[jvs] %s node %u len %u:", tag, p[0], len);
    for (unsigned i = 0; i < len && i < 40 && at < 470; i++)
        at += snprintf(line + at, sizeof line - at, " %02X", p[off + i]);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* ---- the I/O board itself ----
 *
 * JVS is a small request/response protocol over a serial line. A frame is
 *
 *     E0 <dest> <count> <payload...> <sum>
 *
 * where count covers everything after itself including the checksum, and the
 * checksum is the bytes from <dest> to the last payload byte, added up and
 * truncated to eight bits. A reply is the same shape addressed to 00, with a
 * status byte ahead of the reports and one report per command asked.
 *
 * The packet object carries more than the frame. Bytes 1.. of a REQUEST hold
 * a zero-terminated list of where each command starts inside it, built as the
 * game appended them; the matching ACK has to come back with the same list
 * pointing at where each report starts. That list is why this does not need
 * to know how long any command is - the game already said.
 *
 * Nothing here is pressed, inserted or aimed. An attract mode wants a board
 * that answers, not one that plays.
 */

#define JVS_SYNC      0xE0u
#define JVS_STATUS_OK 0x01u
#define JVS_REPORT_OK 0x01u

#define PKT_INDEX     1u        /* command / report offsets, zero-terminated */
#define PKT_FRAME_LEN 0x101u
#define PKT_FRAME     0x102u

/* What the board calls itself. The game reads this back as a C string and
 * shows it in the test menu; the shape is the one every SEGA I/O board of
 * this era reports. */
static const char JVS_IO_IDENT[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Switches, coins, analog channels, and general purpose output - enough for a
 * two player gun cabinet. Each block is kind, then three parameters. */
static const unsigned char JVS_FEATURES[] = {
    0x01, 2, 12, 0,        /* players, buttons each */
    0x02, 2,  0, 0,        /* coin slots */
    0x03, 8, 16, 0,        /* analog channels, bits each */
    0x12, 6,  0, 0,        /* general purpose output */
    0x00                   /* end of list */
};

/* Where the cabinet's reload input lands in a player's switch bytes: byte 0,
 * bit 0 by default; The House of the Dead 4 Special's guns report a shake as
 * the second byte's bit 6. */
static unsigned g_reload_byte, g_reload_mask = 0x01;
void jvs_reload_switch(unsigned byte, unsigned mask) { g_reload_byte = byte; g_reload_mask = mask; }

/* One reply at a time: the game sends, then receives, on the same thread. */
static unsigned char g_ack[0x200];
static unsigned      g_ack_len;
static unsigned char g_report_at[0x40];
static unsigned      g_reports;

static void ack_begin(void)
{
    g_ack_len = 0; g_reports = 0;
    g_ack[g_ack_len++] = JVS_SYNC;
    g_ack[g_ack_len++] = 0x00;              /* addressed to the main board */
    g_ack[g_ack_len++] = 0x00;              /* count, filled in at the end */
    g_ack[g_ack_len++] = JVS_STATUS_OK;
}

/* Remember where this report starts before writing it: that offset is what
 * the game looks up to find the answer to the command it asked. */
static void ack_report(void)
{
    if (g_reports < sizeof g_report_at) g_report_at[g_reports++] = (unsigned char)g_ack_len;
    g_ack[g_ack_len++] = JVS_REPORT_OK;
}

static void ack_byte(unsigned char b)
{
    if (g_ack_len < sizeof g_ack - 1) g_ack[g_ack_len++] = b;
}

static void ack_zeros(unsigned n) { while (n--) ack_byte(0); }

static void ack_end(void)
{
    unsigned sum = 0;
    g_ack[2] = (unsigned char)(g_ack_len - 3 + 1);   /* payload + checksum */
    for (unsigned i = 1; i < g_ack_len; i++) sum += g_ack[i];
    g_ack[g_ack_len++] = (unsigned char)sum;
}

/* Answer one command. `a` is the command byte, `p` its parameters. */
static void jvs_command(const unsigned char *p)
{
    switch (p[0]) {
    case 0x10:                                   /* read board identity */
        ack_report();
        for (const char *s = JVS_IO_IDENT; *s; s++) ack_byte((unsigned char)*s);
        ack_byte(0);
        break;
    case 0x11: ack_report(); ack_byte(0x13); break;   /* command revision */
    case 0x12: ack_report(); ack_byte(0x30); break;   /* JVS revision */
    case 0x13: ack_report(); ack_byte(0x10); break;   /* communication version */
    case 0x14:                                   /* what this board has */
        ack_report();
        for (unsigned i = 0; i < sizeof JVS_FEATURES; i++) ack_byte(JVS_FEATURES[i]);
        break;
    case 0x20: {                                 /* switches: test, then players */
        /* One test byte, then `bytes` bytes for each of `players`. In the
         * first of those, bit 7 is start and bit 1 is the first push button,
         * which for a gun cabinet is the trigger. */
        CabinetInput in;
        host_cabinet_input(&in);
        unsigned players = p[1], bytes = p[2];
        {   /* Confirm the game is actually asking, and with what shape. */
            static int shown;
            if (shown < 3 && getenv("LINDBERGH_JVS")) {
                shown++;
                fprintf(stderr, "[jvs] switch read: %u players x %u bytes\n",
                        players, bytes);
            }
        }
        ack_report();
        ack_byte((in.buttons & CAB_TEST) ? 0x80 : 0x00);
        for (unsigned pl = 0; pl < players; pl++) {
            unsigned char b0 = 0;
            if (pl == 0) {
                if (in.buttons & CAB_P1_START)   b0 |= 0x80;
                if (in.buttons & CAB_SERVICE)    b0 |= 0x40;
                if (in.buttons & CAB_P1_TRIGGER) b0 |= 0x02;
                if ((in.buttons & CAB_P1_RELOAD) && g_reload_byte == 0) b0 |= (unsigned char)g_reload_mask;
            } else if (pl == 1) {
                if (in.buttons & CAB_P2_START)   b0 |= 0x80;
            }
            ack_byte(b0);
            if (bytes > 1) {
                /* second byte: bit 7 is the gun's action button, bit 6 its
                 * fire-mode selector - the masks the game's gun code holds */
                unsigned char b1 = 0;
                if (pl == 0) {        /* the gun's other two buttons */
                    if (in.buttons & CAB_P1_ACTION)   b1 |= 0x80;
                    if (in.buttons & CAB_P1_FIREMODE) b1 |= 0x40;  /* single/burst/auto/grenade */
                    if ((in.buttons & CAB_P1_RELOAD) && g_reload_byte == 1) b1 |= (unsigned char)g_reload_mask;
                }
                ack_byte(b1);
                ack_zeros(bytes - 2);
            }
        }
        host_cabinet_switches_read();
        break;
    }
    case 0x21: {                                 /* coins */
        /* Two bytes a slot: the top two bits are the mechanism's condition,
         * zero meaning normal, and the remaining fourteen are the count. */
        CabinetInput in;
        host_cabinet_input(&in);
        ack_report();
        for (unsigned slot = 0; slot < p[1]; slot++) {
            unsigned n = (slot < 2 ? in.coins[slot] : 0) & 0x3FFF;
            ack_byte((unsigned char)(n >> 8));
            ack_byte((unsigned char)(n & 0xFF));
        }
        break;
    }
    case 0x22: {                                 /* analog: where the gun points */
        CabinetInput in;
        host_cabinet_input(&in);
        int w = in.screen_w > 0 ? in.screen_w : 1;
        int h = in.screen_h > 0 ? in.screen_h : 1;
        unsigned x = (unsigned)((long)in.gun_x * 65535 / w);
        unsigned y = (unsigned)((long)in.gun_y * 65535 / h);
        if (in.gun_x < 0) x = 0; if (x > 65535) x = 65535;
        if (in.gun_y < 0) y = 0; if (y > 65535) y = 65535;
        ack_report();
        {   /* LINDBERGH_NO_GUN=1 reports a centred, still gun. A test, not a
             * setting: it tells apart a game upset by where the gun is from a
             * game upset by something else entirely. */
            static int flat = -1;
            if (flat < 0) { const char *v = getenv("LINDBERGH_NO_GUN");
                            flat = (v && *v && *v != '0') ? 1 : 0; }
            for (unsigned ch = 0; ch < p[1]; ch++) {
                unsigned v = flat ? 0x8000u
                           : (ch == 0 || ch == 2) ? x
                           : (ch == 1 || ch == 3) ? y : 0x8000u;
                /* The game keeps only the first byte: amJvspAckAnalogInput
                 * copies the two bytes as they came and the game reads them
                 * as a little-endian word scaled by 1/256, which is right
                 * only when the second byte is zero - an 8-bit gun. A full
                 * 16-bit value lands the low byte on top and the aim is
                 * noise. */
                ack_byte((unsigned char)(v >> 8));
                ack_byte(0);
            }
        }
        break;
    }
    case 0x23: ack_report(); ack_zeros((unsigned)p[1] * 2u); break;  /* rotary */
    default:                                     /* acknowledged, nothing to say */
        ack_report();
        break;
    }
}

static void h_jvs_send(CPU *c)
{
    uint32_t pkt = A32(0);
    {   static unsigned calls;
        if (getenv("LINDBERGH_JVS") && (++calls <= 4 || calls % 300 == 0))
            fprintf(stderr, "[jvs] send #%u pkt %#x\n", calls, pkt); }
    if (!pkt) { RET(-7); return; }
    const unsigned char *p = (const unsigned char *)GP(pkt);
    const unsigned char *frame = p + 2;

    if (getenv("LINDBERGH_JVS")) jvs_dump("req", pkt, 2, p[1]);

    ack_begin();
    for (unsigned i = 0; i < 0x40 && p[PKT_INDEX + 0x101 + i]; i++) {
        /* The command index lives at 0x102 in the request, where the reply
         * frame lives in an acknowledge packet. Same offset, different use. */
        unsigned off = p[PKT_FRAME + i];
        /* The guard is against running off the packet, not off the frame: a
         * command sits at the end of its frame and its parameters are read
         * past p[1] quite legitimately. Comparing against the frame length
         * rejected every single-command request there is. */
        if (off < 3u || off + 2u >= 0x100u) break;
        jvs_command(frame + off);
    }
    ack_end();
    RET(AM_OK);
}

static void h_jvs_recv(CPU *c)
{
    uint32_t ack = A32(1);
    if (!ack || !g_ack_len) { RET(-6); return; }
    unsigned char *a = (unsigned char *)GP(ack);

    memcpy(a + PKT_FRAME, g_ack, g_ack_len);
    a[PKT_FRAME_LEN] = (unsigned char)g_ack_len;
    /* The report offsets, in the order the commands were asked, zero
     * terminated - the same shape amJvspMakeReportIndex would have left. */
    for (unsigned i = 0; i < g_reports; i++) a[PKT_INDEX + i] = g_report_at[i];
    a[PKT_INDEX + g_reports] = 0;

    if (getenv("LINDBERGH_JVS")) jvs_dump("ack", ack, PKT_FRAME, g_ack_len);
    RET(AM_OK);
}

static int g_country;
static void h_country(CPU *c) { RET(g_country); }

static void h_ok(CPU *c)   { RET(AM_OK); }
static void h_true(CPU *c) { RET(1); }
static void h_one(CPU *c)  { RET(1); }

/* A base board serial, in the shape the cabinet's own would take. */
static void h_serial_id(CPU *c)
{
    static const char id[] = "AAFE-01A00000000";
    uint32_t out = A32(0);
    if (out) memcpy((void *)GP(out), id, sizeof id);
    RET(AM_OK);
}

/* ---- the cabinet's non-volatile memory ----
 *
 * Bookkeeping, settings and credits live in two places on a Lindbergh: a
 * battery-backed SRAM on the base board and a small EEPROM. The game reaches
 * both through four wrapper functions, and everything above them - record
 * layout, duplicate copies, CRCs, validation - is the game's own code, which
 * works perfectly well as long as the bytes come back.
 *
 * So this is the whole of it: two byte arrays that persist to a file. Nothing
 * models a record, because nothing needs to.
 */

#define SRAM_SIZE   0x40000u
#define EEPROM_SIZE 0x10000u

static unsigned char g_sram[SRAM_SIZE];
static unsigned char g_eeprom[EEPROM_SIZE];
static int           g_nvram_loaded;

static const char *nvram_path(void)
{
    static char path[512];
    if (!*path) {
        const char *p = getenv("LINDBERGH_NVRAM");
        snprintf(path, sizeof path, "%s", (p && *p) ? p : "lindbergh_nvram.bin");
    }
    return path;
}

static int g_sysdata_region;    /* the game's region comes from the EEPROM's system data */
static int region_value(void);

/* The EEPROM's static system data record: 32 bytes at its start, a CRC-32
 * (no final inversion) of the other 28 first, the region at byte 14 (0 Japan,
 * 1 USA, 2 export), then the board's serial. A game that takes its region
 * from here (The House of the Dead 4 Special) gets the Language setting
 * written into it; a blank store gets a whole record. */
static void sysdata_apply_region(void)
{
    unsigned char *r = g_eeprom;
    int blank = 1;
    for (int i = 0; i < 32; i++) if (r[i]) { blank = 0; break; }
    if (blank) memcpy(r + 15, "AALG-TG-933F3904", 16);
    r[14] = (unsigned char)region_value();
    uint32_t crc = (uint32_t)crc32(0L, r + 4, 28) ^ 0xFFFFFFFFu;
    memcpy(r, &crc, 4);
    fprintf(stderr, "[sega] system data region %d\n", r[14]);
}

void sega_sysdata_region(int on) { g_sysdata_region = on; }

static void nvram_read(void);
static void nvram_load(void)
{
    if (g_nvram_loaded) return;
    g_nvram_loaded = 1;
    nvram_read();
    if (g_sysdata_region) sysdata_apply_region();
}

static void nvram_read(void)
{
    FILE *f = fopen(nvram_path(), "rb");
    if (!f) return;
    if (fread(g_sram, 1, sizeof g_sram, f) != sizeof g_sram ||
        fread(g_eeprom, 1, sizeof g_eeprom, f) != sizeof g_eeprom) {
        /* A short or corrupt file is not worth keeping: the game rebuilds its
         * records from defaults the moment it finds them invalid. */
        memset(g_sram, 0, sizeof g_sram);
        memset(g_eeprom, 0, sizeof g_eeprom);
    }
    fclose(f);
}

static void nvram_save(void)
{
    FILE *f = fopen(nvram_path(), "wb");
    if (!f) return;
    fwrite(g_sram, 1, sizeof g_sram, f);
    fwrite(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
}

/* The wrappers answer 0, or -12 the way the real ones do when the base board
 * does not reply. An access outside the store is that failure. */
#define AM_BACKUP_FAIL (-12)

static int nvram_copy(unsigned char *store, unsigned size, uint32_t guest,
                      uint32_t addr, uint32_t len, int writing)
{
    nvram_load();
    if (!guest || addr > size || len > size - addr) return AM_BACKUP_FAIL;
    void *g = (void *)GP(guest);
    if (writing) { memcpy(store + addr, g, len); nvram_save(); }
    else           memcpy(g, store + addr, len);
    return AM_OK;
}

/* Each wrapper reorders its arguments on the way down to a different device,
 * so which of the three is the buffer is not the same for any two of them.
 * LINDBERGH_NVLOG=1 prints all three and lets their magnitude say: a guest
 * pointer is up in the image, an offset is small. Read takes the offset
 * first, write takes the buffer first - reading both the same way puts a
 * pointer's value in as an address and reports a broken record. */
static void nv_log(const char *who, CPU *c)
{
    if (getenv("LINDBERGH_NVLOG"))
        fprintf(stderr, "[nv] %-10s a0=0x%08X a1=0x%08X a2=0x%08X\n",
                who, A32(0), A32(1), A32(2));
}

/* The EEPROM, reached directly rather than through the backup record layer.
 *
 * OutRun 2 SP SDX saves its assignment data by calling amEepromRead and
 * amEepromWrite itself. Their argument orders differ from each other the same
 * way the SRAM wrappers do - read takes the address first, write takes the
 * buffer first - and the address is a sixteen bit word in both. */
static void h_eeprom_read_api(CPU *c)
{
    nv_log("eepromRead", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0) & 0xFFFFu, A32(2), 0));
}

static void h_eeprom_write_api(CPU *c)
{
    nv_log("eepromWrite", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1) & 0xFFFFu, A32(2), 1));
}

static void h_sram_read(CPU *c)
{ nv_log("sramRead", c);  RET(nvram_copy(g_sram, SRAM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_sram_write(CPU *c)
{ nv_log("sramWrite", c); RET(nvram_copy(g_sram, SRAM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* The EEPROM pair reorders differently again and has not been reached yet;
 * these follow the SRAM shape and the log will say if that is wrong. */
static void h_eeprom_read(CPU *c)
{ nv_log("eepRead", c);   RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_eeprom_write(CPU *c)
{ nv_log("eepWrite", c);  RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* ---- the error the blank store raises ----
 *
 * acpSystem keeps one error code in a global and paints it over the screen
 * once set; anything other than -1 or -2 comes out as "Error 15 - Game
 * Program Not Found". The backup layer sets -3 when its records fail their
 * CRC, which they do here because the store is blank: the game's own repair
 * path writes through an EEPROM that lives on an I2C bus this runtime does
 * not have, so the records can never become valid and the error is permanent.
 *
 * A cabinet ships with that store already written. Suppressing this one code
 * is the same statement - the bookkeeping is assumed good - and it is scoped
 * to exactly that: every other error still reaches the game untouched, and
 * the suppression announces itself once on the way past.
 *
 * setError is reimplemented rather than skipped, which means knowing the two
 * globals it touches. Those are read out of the function's own first bytes
 * instead of being written down here, so this survives a different build of
 * the same framework:
 *
 *     55                 push ebp
 *     A1 <error var>     mov  eax, [error var]
 *     80 3D <latch> 00   cmp  byte [latch], 0
 *     ...
 *     A3/89 05 <var>     mov  [error var], eax
 */
#define ACP_ERR_BACKUP (-3)

static uint32_t g_err_var, g_err_latch;

static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_error_globals(uint32_t va)
{
    const unsigned char *p = (const unsigned char *)GP(va);
    for (unsigned i = 0; i < 24; i++) {
        if (!g_err_var && p[i] == 0xA1) g_err_var = rd32le(p + i + 1);
        if (!g_err_latch && p[i] == 0x80 && p[i + 1] == 0x3D)
            g_err_latch = rd32le(p + i + 2);
    }
    return g_err_var != 0;
}

static void h_set_error(CPU *c)
{
    int32_t v = (int32_t)A32(0);

    if (v == ACP_ERR_BACKUP) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[sega] backup records are blank and cannot be "
                            "initialised without the EEPROM bus; treating the "
                            "bookkeeping as good\n");
        }
        RET(0);
        return;
    }

    /* Faithful otherwise: the original only stores while the latch is clear,
     * so that the first error to happen is the one that gets reported. */
    if (g_err_var &&
        (!g_err_latch || *(const unsigned char *)GP(g_err_latch) == 0))
        *(uint32_t *)GP(g_err_var) = (uint32_t)v;

    if (getenv("LINDBERGH_ACPERR")) {
        fprintf(stderr, "[acp] setError(%d) from:\n", (int)v);
        guest_backtrace(c);
    }
    RET(0);
}

/* ---- the security dongle ----
 *
 * A Lindbergh cabinet carries a hardware dongle, and a game does not simply
 * ask whether it is present. Ghost Squad Evolution generates sixteen random
 * bytes, has the dongle transform them, and checks the answer. The key for
 * that transform lives inside the device; it is not in the disc, not in the
 * binary, and not recoverable from either.
 *
 * So there is nothing to emulate here and no cleverness that would help. The
 * cabinet this game ran on had the dongle fitted, and the runtime makes the
 * same statement - the licence is present - by answering the one predicate
 * the game gates on, and saying so once on the way past. The challenge and
 * response themselves are left entirely alone.
 *
 * Bound by symbol name, so it applies to a game that has these and is simply
 * absent from one that does not. */
static void h_dongle_valid(CPU *c)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[sega] no security dongle is fitted; treating the "
                        "licence as present\n");
    }
    RET(1);
}

/* amDipswGetData(unsigned char *out): the baseboard's DIP switches. On a
 * Lindbergh, switches 6-8 (bits 5-7) select the monitor's resolution, and a
 * game sizes everything it draws from them; with no answer it falls back to
 * 640x480 whatever the window is. So the bits are the window's size:
 *   0x00 640x480   0x20 800x480   0x40 1024x768  0x60 1280x768
 *   0x80 800x600   0xA0 1024x600  0xC0 1280x1024 0xE0 1280x720 (1360x768
 *   to House of the Dead 4, which renders and aims for that)
 * LINDBERGH_DIPSW=<hex> sets the whole byte instead. */
static void h_dipsw_get(CPU *c)
{
    static int dips = -1;
    if (dips < 0) {
        const char *v = getenv("LINDBERGH_DIPSW");
        if (v && *v) dips = (int)strtol(v, NULL, 16) & 0xFF;
        else {
            static const struct { int w, h, bits; } res[] = {
                { 640, 480, 0x00 }, { 800, 480, 0x20 }, { 1024, 768, 0x40 }, { 1280, 768, 0x60 },
                { 800, 600, 0x80 }, { 1024, 600, 0xA0 }, { 1280, 1024, 0xC0 }, { 1280, 720, 0xE0 },
                { 1360, 768, 0xE0 },    /* what 0xE0 is to House of the Dead 4 */
            };
            int w, h;
            lindbergh_window_size(&w, &h);
            dips = 0x60;
            for (unsigned i = 0; i < sizeof res / sizeof *res; i++)
                if (res[i].w == w && res[i].h == h) dips = res[i].bits;
        }
        fprintf(stderr, "[sega] DIP switches %#04x\n", dips);
    }
    if (A32(0)) *(uint8_t *)APTR(0) = (uint8_t)dips;
    RET(0);
}

static int region_value(void)
{
    const char *v = getenv("LINDBERGH_REGION");
    if (v && *v) return atoi(v);
    return g_country ? 1 : 0;
}
static void h_region(CPU *c) { RET(region_value()); }
#if defined(__ANDROID__)
/* Ghost Squad Evolution paces itself: idle_wait_func spins on the wall clock
 * until 16.683 ms (59.94 Hz) have passed since the frame began. The headset
 * shows 120 Hz, two display frames per game frame at exactly 60: at 59.94 the
 * game slid through the display's phase, and each time it crossed a display
 * frame one game frame was shown once and the next three times - judder.
 * The frame target is read only by the waits there, so for the call it is
 * the display's own (host_frame_period_ms: the headset's period times its
 * frames per game frame). Pacing by the display alone was tried: the game
 * then begins a frame only when the headset releases it, too late to finish
 * in two display frames. The same constant elsewhere (main_loop_func's frame
 * ratios) is untouched. LINDBERGH_GAME_PACING=1 keeps the game's own. */
#define FRAME_TARGET_VA 0x081c1d40u
static void (*g_idle_wait)(CPU *);
static void h_idle_wait(CPU *c)
{
    uint32_t keep = rd32(FRAME_TARGET_VA), now;
    extern float host_frame_period_ms(void);
    float t = host_frame_period_ms();
    if (!(t > 5.0f && t < 40.0f)) memcpy(&t, &keep, 4);
    memcpy(&now, &t, 4);
    wr32(FRAME_TARGET_VA, now);
    push32(c, 0xDEADBEEFu);                 /* the return slot its ret pops */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_idle_wait(c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    wr32(FRAME_TARGET_VA, keep);
    {   /* headroom: how long the frame waited, for the 5-second report */
        static double sum, lo = 1e9; static unsigned n, tight;
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        sum += ms; if (ms < lo) lo = ms; if (ms < 1.0) tight++;
        if (++n == 300) {
            fprintf(stderr, "[idle] frame wait: mean %.1f ms, least %.1f ms, under 1 ms %u of 300\n", sum / n, lo, tight);
            sum = 0; lo = 1e9; n = 0; tight = 0;
        }
    }
}
static int pace_by_display(void)
{
    const char *v = getenv("LINDBERGH_GAME_PACING");
    if (v && *v == '1') return 0;
    uint32_t va = guest_symbol("_Z14idle_wait_funcv");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    uint32_t bits = va ? rd32(FRAME_TARGET_VA) : 0;
    float target;
    memcpy(&target, &bits, 4);
    if (!va || target < 16.6f || target > 16.8f || !(g_idle_wait = dispatch_lifted(va))) return 0;
    return guest_override("_Z14idle_wait_funcv", h_idle_wait);
}
#endif

/* The game culls its models against a viewing volume as wide as its camera's
 * lens (dxMakePerspectiveViewingVolume, from set_camera_transform). In the
 * headset the view is far wider than the arcade monitor - about 100 degrees,
 * and the head turns - so a soldier who steps close and to the side was
 * dropped while still in view. In VR the volume is built wider than the
 * camera (LINDBERGH_CULL_FOV, degrees, vertical; 0 keeps the game's). Only
 * the culling is widened: the projection is set elsewhere and unchanged. */
static void (*g_view_volume)(CPU *);
static float g_cull_fov;
static void h_view_volume(CPU *c)
{
    uint32_t bits = A32(0);
    float fov;
    memcpy(&fov, &bits, 4);
    static int force = -1;               /* LINDBERGH_CULL_FORCE=1: exactly that angle, narrower too (tests) */
    if (force < 0) { const char *f = getenv("LINDBERGH_CULL_FORCE"); force = f && *f == '1'; }
    if (fov > 0 && (fov < g_cull_fov || force)) {
        memcpy(&bits, &g_cull_fov, 4);
        wr32(c->esp, bits);             /* the caller's argument, its own to clean */
    }
    push32(c, 0xDEADBEEFu);             /* the return slot its ret pops */
    g_view_volume(c);
}
static int widen_culling(void)
{
    const char *v = getenv("LINDBERGH_CULL_FOV");
    g_cull_fov = v ? (float)atof(v) : 0.0f;  /* off unless asked for */
    if (!(g_cull_fov > 0 && g_cull_fov < 179)) return 0;
    uint32_t va = guest_symbol("_Z30dxMakePerspectiveViewingVolumeffff");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    if (!va || !(g_view_volume = dispatch_lifted(va))) return 0;
    return guest_override("_Z30dxMakePerspectiveViewingVolumeffff", h_view_volume);
}

/* LINDBERGH_GOD=1: the player takes no damage - for test runs that must
 * reach the end of a mission. */
static void h_no_damage(CPU *c) { RET(0); }

/* The player's route. A mission is a chain of routes (a static table of 76,
 * each a path, the next route and an area); the player's record holds the
 * current one. LINDBERGH_ROUTE_LOG=1 reports each change; LINDBERGH_ROUTE=N
 * puts the player on route N a second into the mission - a jump, e.g. to
 * a mission's last scene. */
#define PLAYER0_VA   0x086617e0u
#define ROUTE_TABLE  0x08276ea0u
static void (*g_game_loop)(CPU *);
static int g_route_jump = -1, g_route_log;
static int player_route(void)
{
    uint32_t pw = rd32(PLAYER0_VA + 0xf4);
    if (!pw) return -1;
    uint32_t r = rd32(pw + 0x100);
    for (int i = 0; r && i < 76; i++) if (rd32(ROUTE_TABLE + 4u * i) == r) return i;
    return -1;
}
static void h_game_loop(CPU *c)
{
    static int last = -2, loops;
    extern unsigned g_frame;
    int r = player_route();
    if (g_route_log && r != last)
        fprintf(stderr, "[route] frame %u: route %d (path %d, area %d)\n", g_frame, r,
                r >= 0 ? (int)rd32(rd32(ROUTE_TABLE + 4u * r) + 4) : -1, r >= 0 ? (int)rd32(rd32(ROUTE_TABLE + 4u * r) + 16) : -1);
    last = r;
    if (r >= 0) loops++; else loops = 0;
    {   /* LINDBERGH_HURT=N (test): the player takes a hit every N frames of play */
        static int hurt = -1;
        if (hurt < 0) { const char *h = getenv("LINDBERGH_HURT"); hurt = h ? atoi(h) : 0; }
        if (hurt > 0 && loops > 0 && loops % hurt == 0) {
            uint32_t fn = guest_symbol("_Z17set_player_damageiiii");
            uint32_t a[4] = { 0, 1, 0, 0 };
            if (fn) guest_call(c, fn, a, 4);
            fprintf(stderr, "[sega] frame %u: test hit\n", g_frame);
        }
    }
    if (g_route_jump >= 0 && loops == 60) {
        uint32_t fn = guest_symbol("_Z19set_player_route_idii");
        uint32_t a[2] = { 0, (uint32_t)g_route_jump };
        uint32_t ok = fn ? guest_call(c, fn, a, 2) : 0;
        uint32_t path = rd32(rd32(PLAYER0_VA + 0xf4) + 0x108);    /* its path: start and end frame */
        int p0 = path ? (int)rd32(path) : 0, p1 = path ? (int)rd32(path + 4) : 0;
        fprintf(stderr, "[route] frame %u: jumped to route %d (%s), its path frames %d..%d\n", g_frame, g_route_jump, ok ? "ok" : "refused", p0, p1);
        g_route_jump = -1;
        const char *pf = getenv("LINDBERGH_PATHFRAME");   /* and this far along it (negative: from its end) */
        uint32_t fs = guest_symbol("_Z20set_player_pathframeif");
        if (ok && pf && *pf && fs) {
            float f = (float)atof(pf);
            f += f < 0 ? p1 : p0;
            uint32_t b2[2] = { 0, 0 };
            memcpy(&b2[1], &f, 4);
            guest_call(c, fs, b2, 2);
            fprintf(stderr, "[route] frame %u: path frame set to %.0f\n", g_frame, f);
        }
    }
    {   /* LINDBERGH_SHIMMER=<strength>: the plane finale's air shimmer, in any scene - for testing it */
        static float sh = -1;
        if (sh < 0) { const char *v = getenv("LINDBERGH_SHIMMER"); sh = v && *v ? (float)atof(v) : 0; }
        if (sh > 0 && loops >= 60) {
            uint32_t g = guest_symbol("_Z12efcSetGlobaliifPv"), w = guest_symbol("_Z17efcSetShimmerWarpf");
            float one = 1.0f; uint32_t a[4] = { 7, 1, 0, 0 }, b1[1];
            memcpy(&a[2], &one, 4); memcpy(&b1[0], &sh, 4);
            if (loops == 60 && g) fprintf(stderr, "[sega] shimmer on: %u\n", guest_call(c, g, a, 4));
            if (w) guest_call(c, w, b1, 1);
        }
    }
    {   /* LINDBERGH_GUNACT_LOG=1: player 1's gun - its input words (held,
         * pressed) and the 16 gun actions the game derives from them
         * (is_gun_action; CQC asks for action 14) - on every change */
        static int on = -1;
        if (on < 0) { const char *v = getenv("LINDBERGH_GUNACT_LOG"); on = v && *v == '1'; }
        if (on) {
            enum { GUN0 = 0x8651e60 };
            uint32_t in = rd32(GUN0 + 0x40);
            uint32_t now[4] = { rd32(GUN0 + 0x64), in ? rd32(in + 4) : 0, in ? rd32(in + 8) : 0, rd32(GUN0 + 0x4) };
            static uint32_t last[4];
            if (memcmp(now, last, sizeof now)) {
                fprintf(stderr, "[gunact] frame %u: actions %#x, held %#x, pressed %#x, gun flags %#x\n",
                        g_frame, now[0], now[1], now[2], now[3]);
                memcpy(last, now, sizeof now);
            }
        }
    }
    push32(c, 0xDEADBEEFu);
    g_game_loop(c);
}
/* The sniping and CQC events always report to stereo.c: they change its
 * scope test and how the gun is aimed. LINDBERGH_QTE_LOG=1: each frame of CQC (qte_ctrl_grapple) and of the
 * sniping events (qte_ctrl_snipe): the markers CQC checks the gun against,
 * in screen coordinates, and where the cabinet says the gun points. */
static void (*g_qte_grapple)(CPU *), (*g_qte_snipe)(CPU *);
static void qte_log(const char *what, uint32_t q)
{
    extern unsigned g_frame;
    CabinetInput in;
    host_cabinet_input(&in);
    char marks[256] = "";
    int n = (int)rd32(q + 0x38), len = 0;
    for (int i = 0; i < n && i < 8 && len < 200; i++) {
        uint32_t e = q + 0x40 + 0x14u * i;
        len += snprintf(marks + len, sizeof marks - len, " %d@%d,%d", (int16_t)rd32(e) , (int16_t)rd32(e + 4), (int16_t)rd32(e + 6));
    }
    fprintf(stderr, "[qte] frame %u: %s, state %d, gun %d,%d buttons %#x, %d marks:%s\n", g_frame, what,
            (int)rd32(q + 4), in.gun_x, in.gun_y, in.buttons, n, marks);
}
static int g_qte_log;
static void h_qte_grapple(CPU *c)
{
#ifndef _WIN32
    stereo_cqc_running();
#endif
    if (g_qte_log) qte_log("CQC", A32(0));
    push32(c, 0xDEADBEEFu);
    g_qte_grapple(c);
}
static void h_qte_snipe(CPU *c)
{
#ifndef _WIN32
    stereo_snipe_running();
#endif
    if (g_qte_log) qte_log("snipe", A32(0));
    push32(c, 0xDEADBEEFu);
    g_qte_snipe(c);
}
static int watch_qte(void)
{
    const char *v = getenv("LINDBERGH_QTE_LOG");
    g_qte_log = v && *v == '1';
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    uint32_t g = guest_symbol("_Z16qte_ctrl_grappleP3QTE"), s = guest_symbol("_Z14qte_ctrl_snipeP3QTE");
    int n = 0;
    if (g && (g_qte_grapple = dispatch_lifted(g))) n += guest_override("_Z16qte_ctrl_grappleP3QTE", h_qte_grapple);
    if (s && (g_qte_snipe = dispatch_lifted(s))) n += guest_override("_Z14qte_ctrl_snipeP3QTE", h_qte_snipe);
    return n;
}

static int watch_route(void)
{
    const char *j = getenv("LINDBERGH_ROUTE"), *l = getenv("LINDBERGH_ROUTE_LOG");
    g_route_log = l && *l == '1';
    if (j && *j) g_route_jump = atoi(j);
    if (!g_route_log && g_route_jump < 0 && !getenv("LINDBERGH_SHIMMER") && !getenv("LINDBERGH_HURT") && !getenv("LINDBERGH_GUNACT_LOG")) return 0;
    uint32_t va = guest_symbol("_Z8GameLoopP8GAMEWORK");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    if (!va || !(g_game_loop = dispatch_lifted(va))) return 0;
    return guest_override("_Z8GameLoopP8GAMEWORK", h_game_loop);
}

void hle_register_sega(void)
{
    int n = 0;
    if (watch_route()) fprintf(stderr, "[sega] route log/jump on\n");
    if (watch_qte()) fprintf(stderr, "[sega] sniping events watched%s\n", g_qte_log ? ", CQC and sniping logged" : "");
    {
        const char *g = getenv("LINDBERGH_GOD");
        if (g && *g == '1' && (guest_override("_Z26check_player_damage_enablei", h_no_damage) |
                               guest_override("_Z21PlMgr_CanDamagePlayeri", h_no_damage)))  /* The House of the Dead 4 */
            fprintf(stderr, "[sega] the player takes no damage (LINDBERGH_GOD)\n");
    }
    if (widen_culling()) fprintf(stderr, "[sega] model culling widened to %.0f degrees\n", g_cull_fov);
#if defined(__ANDROID__)
    if (pace_by_display()) fprintf(stderr, "[sega] idle_wait_func: paced by the headset, not the wall clock\n");
#endif

    /* Ghost Squad Evolution gates its whole boot on this one predicate;
     * a game without these symbols simply does not get the override. */
    n += guest_override("_Z18is_dongle_validatev", h_dongle_valid);
    n += guest_override("_Z21check_dongle_validatev", h_dongle_valid);

    /* The base board. amLibInit failing is what produces "SEGA BaseBD not
     * available", and everything else is gated behind it. */
    n += guest_override("amLibInit", h_ok);
    n += guest_override("amLibExit", h_ok);
    n += guest_override("amLibIsBasebdAvailable", h_true);

    /* The JVS link itself. amJvsCheckInit is a predicate, not a status code:
     * amJvsGetNodes reads it as a boolean and answers -5 when it is false,
     * which is how answering it with the library's success value of 0 turned
     * into "-5 JVS node(s) found". */
    n += guest_override("amJvsInit", h_ok);
    n += guest_override("amJvsExit", h_ok);
    n += guest_override("amJvsCheckInit", h_true);

    /* One I/O board on the link - the cabinet's own, with the buttons, the
     * coin slots and the guns. */
    n += guest_override("amJvsGetNodes", h_one);

    /* The security dongle. Nothing here reads a real one, and the game only
     * asks whether it is present before carrying on. */
    n += guest_override("amDongleInit", h_ok);
    n += guest_override("amDongleExit", h_ok);
    n += guest_override("amDongleIsAvailable", h_true);

    /* The dipswitches and the bookkeeping the game reads at start up. It
     * asks for these by name in its console when they are missing. */
    n += guest_override("amDipswInit", h_ok);
    n += guest_override("amDipswExit", h_ok);
    n += guest_override("amDipswGetData", h_dipsw_get);

    /* acpSystem::checkDongle polls amDongleUpdate and raises "Error 15 -
     * Game Program Not Found" the moment it answers anything but zero. */
    n += guest_override("amDongleUpdate", h_ok);

    /* The base board's serial. The game prints it and stores it with its
     * bookkeeping; nothing here depends on the value, only on having one. */
    n += guest_override("amOsinfoGetBaseBoardSerialId", h_serial_id);

    /* The battery-backed store the bookkeeping lives in. */
    n += guest_override("amEepromInit", h_ok);
    n += guest_override("amEepromExit", h_ok);
    n += guest_override("amBackupWrapper_BbBuSramRead", h_sram_read);
    n += guest_override("amBackupWrapper_BbBuSramWrite", h_sram_write);
    n += guest_override("amBackupWrapper_KeyEepromRead", h_eeprom_read);
    n += guest_override("amBackupWrapper_KeyEepromWrite", h_eeprom_write);
    n += guest_override("amEepromRead", h_eeprom_read_api);
    n += guest_override("amEepromWrite", h_eeprom_write_api);

    /* LINDBERGH_NO_JVS=1 leaves the board unanswered, so the game finds no
     * I/O and says so. A test for whether a fault lives in this transport or
     * merely happens to be near it. */
    if (!getenv("LINDBERGH_NO_JVS")) {
        n += guest_override("amJvsSendRequest", h_jvs_send);
        n += guest_override("amJvsRecvAcknowledge", h_jvs_recv);
    }

    {   /* Only worth installing if the two globals can be found; without
         * them this would swallow every error instead of one. */
        uint32_t va = guest_symbol("_ZN9acpSystem8setErrorEl");
        if (va && find_error_globals(va))
            n += guest_override("_ZN9acpSystem8setErrorEl", h_set_error);
    }

    /* The country, and with it the language: the game reads the region byte
     * of the cabinet's system data (0, Japan, on a Japanese board) and shows
     * Japanese for country 0, English otherwise. The Language menu picks it
     * (default English); it applies from the next start. */
    {
#if defined(_WIN32)
        { const char *v = getenv("LINDBERGH_COUNTRY"); g_country = v && *v ? atoi(v) : 1; }
#else
        g_country = settings()->country;     /* LINDBERGH_COUNTRY overrides */
#endif
        if (guest_override("_Z25get_arcade_region_countryv", h_country)) {
            n++;
            fprintf(stderr, "[sega] country %d (%s)\n", g_country, g_country ? "English" : "Japanese");
        }
        /* The House of the Dead 4 asks CConfigMgr::GetRegion - 0 Japan,
         * 1 USA, 2 export - and shows English for anything but Japan. */
        if (guest_override("_ZN10CConfigMgr9GetRegionEv", h_region)) {
            n++;
            fprintf(stderr, "[sega] region %d\n", region_value());
        }
    }

    fprintf(stderr, "[sega] %d base board entry points answered\n", n);
}
