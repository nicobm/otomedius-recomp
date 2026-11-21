/* card.c - virtual e-Amusement card (offline replacement of the card service).
 *
 * The cabinet read a player's e-Amusement card through the ICCA reader
 * (libacio) and kept the profile on Konami's servers (libbms cardmng /
 * playerdata).  With OTOMEDIUS_CARD set, a virtual card is "inserted" whenever
 * the game asks for one and the player data is kept in files next to the game:
 *
 *     OTOMEDIUS_CARD=1                  card E004010000000001
 *     OTOMEDIUS_CARD=E004010000000002   any 16 hex digits (one folder per card)
 *     OTOMEDIUS_CARD_PIN=1              keep the 4-digit PIN entry (skipped by default)
 *
 * Data: <game dir>/card/<id>/card.ini (registered flag), profile.bin and
 * char<N>.bin (one 0x6B8-byte record each).  Without OTOMEDIUS_CARD nothing
 * changes: no card is ever read, as before.
 *
 * Findings (static analysis, verified on the Windows loader of the same
 * binary, otomedius-pc v0.6.1):
 * - Reader: the ICCA state machine 0x4132C0 (state 5) polls
 *   ac_io_icca_req_uid_isfinished(unit, &status); status 9 = card read,
 *   10 = no card.  ac_io_icca_get_uid(unit, uid) then copies the 8-byte id.
 * - bms_cardmng_inquire_wait(?, &status, info[2]): status 0 + info {1, 0} =
 *   registered card (0x41FF60 -> PIN entry 0x420F70 -> playerdata_load);
 *   status 3 = new card (registration scenes).  Every other cardmng step
 *   reports status 0 = done (0x420F70, 0x4209D0, 0x4202F0, 0x421A20).
 * - Player data: the manager object (0x1B10 bytes) holds the phase at +0x20,
 *   a record count at +0x24, a character mask at +0x28 and the data area at
 *   +0x30 (= the buffer passed to playerdata_save/_load), in records of 0x6B8
 *   bytes.  Save (builder 0x422D50): phase 0 = profile record, plus the
 *   played character's record (index at 0x5DB878) when the count is 2; later
 *   phases (0x4233C0) hold characters in the order of the mask bits.  Load
 *   (parser 0x4234A0) asks the same way; a record of zeros means "no data"
 *   (new player).
 * - PIN: the login machine 0x420F70 switches on [ebx+0x29D] through the table
 *   at 0x42140C; state 2 (0x4210F3) is the PIN keypad, which ends at
 *   0x42116E by setting state 3 (login).  card_skip_pin() lets patches.c do
 *   that directly.
 */

#define _GNU_SOURCE
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REC        0x6B8u      /* one profile or character record */
#define DATA_SIZE  0x1AE0u     /* data area of the player-data manager */
#define CUR_CHAR   0x5DB878u   /* index of the character being played */

static int  g_init, g_on, g_pin;
static char g_id[17];
static char g_dir[4096];
static uint8_t g_uid[8];

static void card_init(void)
{
    const char *e = getenv("OTOMEDIUS_CARD"), *p = getenv("OTOMEDIUS_CARD_PIN");
    char base[4096];
    g_init = 1;
    if (!e || !*e || !strcmp(e, "0"))
        return;
    snprintf(g_id, sizeof(g_id), "%s", strlen(e) == 16 ? e : "E004010000000001");
    for (int i = 0; i < 8; i++) {
        unsigned v = 0;
        if (sscanf(g_id + 2 * i, "%2x", &v) != 1)
            v = 0;
        g_uid[i] = (uint8_t)v;
    }
    g_on = 1;
    g_pin = p && *p && *p != '0';
    snprintf(base, sizeof(base), "%s/card", win_game_dir());
    mkdir(base, 0755);
    snprintf(g_dir, sizeof(g_dir), "%s/%s", base, g_id);
    mkdir(g_dir, 0755);
    log_printf("card: virtual card %s, data in %s%s", g_id, g_dir,
               g_pin ? "" : " (PIN entry skipped)");
}

int card_enabled(void)
{
    if (!g_init)
        card_init();
    return g_on;
}

int card_skip_pin(void) { return card_enabled() && !g_pin; }

static void card_path(char *f, size_t n, const char *name)
{
    snprintf(f, n, "%s/%s", g_dir, name);
}

static int registered(void)
{
    char f[4200], line[64];
    int r = 0;
    card_path(f, sizeof(f), "card.ini");
    FILE *fp = fopen(f, "r");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp))
        if (!strncmp(line, "registered=1", 12))
            r = 1;
    fclose(fp);
    return r;
}

static void set_registered(void)
{
    char f[4200];
    if (!card_enabled() || registered())
        return;
    card_path(f, sizeof(f), "card.ini");
    FILE *fp = fopen(f, "w");
    if (fp) {
        fputs("[card]\nregistered=1\n", fp);
        fclose(fp);
    }
}

static void rec_name(char *f, size_t n, int ch)
{
    char name[32];
    if (ch < 0)
        snprintf(name, sizeof(name), "profile.bin");
    else
        snprintf(name, sizeof(name), "char%d.bin", ch);
    card_path(f, n, name);
}

static void rec_write(int ch, uint32_t src)
{
    char f[4200];
    rec_name(f, sizeof(f), ch);
    FILE *fp = fopen(f, "wb");
    if (fp) {
        fwrite(MEM + src, 1, REC, fp);
        fclose(fp);
    }
    log_printf("card:   %s -> %s", ch < 0 ? "profile" : "character",
               fp ? f : "WRITE FAILED");
}

static int rec_read(int ch, uint32_t dst)
{
    char f[4200];
    int got = 0;
    rec_name(f, sizeof(f), ch);
    memset(MEM + dst, 0, REC);
    FILE *fp = fopen(f, "rb");
    if (fp) {
        got = fread(MEM + dst, 1, REC, fp) == REC;
        fclose(fp);
    }
    return got;
}

/* ---------------------------------------------------------- ICCA reader */
/* ac_io_icca_req_uid_isfinished(unit, int *status) */
void card_icca_req_uid_fin(void)
{
    uint32_t st = arg32(1);
    if (st)
        M32(st) = card_enabled() ? 9u : 10u;   /* 9 = card read, 10 = no card */
    RET32(1);
}

/* ac_io_icca_get_uid(unit, uint8_t uid[8]) */
void card_icca_get_uid(void)
{
    uint32_t p = arg32(1);
    if (!card_enabled() || !p) {
        RET32(0);
        return;
    }
    memcpy(MEM + p, g_uid, 8);
    log_printf("card: uid read");
    RET32(1);
}

/* --------------------------------------------------------- libbms cardmng */
/* bms_cardmng_inquire_wait(?, int *status, int info[2]) */
void card_bms_inquire_wait(void)
{
    uint32_t st = arg32(1), info = arg32(2);
    int reg = card_enabled() && registered();
    if (st)
        M32(st) = reg ? 0u : 3u;
    if (info) {
        M32(info) = reg ? 1u : 0u;
        M32(info + 4) = 0;
    }
    log_printf("card: inquire -> %s", reg ? "registered" : "new card");
    RET32(0);
}

/* the other *_wait(?, int *status) steps: done */
void card_bms_wait_ok(void)
{
    uint32_t st = arg32(1);
    if (st)
        M32(st) = 0;
    RET32(0);
}

/* cardactivate / bindcard: the card now has an account */
void card_bms_wait_register(void)
{
    set_registered();
    card_bms_wait_ok();
}

/* bms_playerdata_save(?, buf, size) */
void card_bms_save(void)
{
    uint32_t buf = arg32(1);
    if (card_enabled() && buf) {
        uint32_t obj = buf - 0x30u;
        int ph = (int)M32(obj + 0x20), cnt = (int)M32(obj + 0x24);
        uint8_t m = M8(obj + 0x28), ch = M8(CUR_CHAR);
        log_printf("card: save phase %d, %d record(s), mask %02x, character %d",
                   ph, cnt, m, ch);
        if (ph == 0) {
            if (M8(buf))
                rec_write(-1, buf);
            if (cnt >= 2 && M8(buf + REC) && ch < 8)
                rec_write(ch, buf + REC);
        } else {
            int k = 0;
            for (int bit = 0; bit < 8 && k < cnt && k < 4; bit++)
                if (m & (1u << bit)) {
                    if (M8(buf + k * REC))
                        rec_write(bit, buf + k * REC);
                    k++;
                }
        }
        set_registered();
    }
    RET32(0);
}

/* bms_playerdata_load(?, buf, size) */
void card_bms_load(void)
{
    uint32_t buf = arg32(1);
    if (buf) {
        uint32_t obj = buf - 0x30u;
        int ph = (int)M32(obj + 0x20), cnt = (int)M32(obj + 0x24), got = 0;
        uint8_t m = M8(obj + 0x28);
        memset(MEM + buf, 0, DATA_SIZE);
        if (card_enabled()) {
            if (ph == 0)
                got = rec_read(-1, buf);
            else {
                int k = 0;
                for (int bit = 0; bit < 8 && k < cnt && k < 4; bit++)
                    if (m & (1u << bit)) {
                        got += rec_read(bit, buf + k * REC);
                        k++;
                    }
            }
            log_printf("card: load phase %d, %d record(s), mask %02x -> %d found",
                       ph, cnt, m, got);
        }
    }
    RET32(0);
}

/* bms_playerdata_{save,load}_wait(int *result) */
void card_bms_data_wait(void)
{
    uint32_t r = arg32(0);
    if (r)
        M32(r) = 0;
    RET32(0);
}
