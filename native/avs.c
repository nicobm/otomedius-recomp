/* avs.c - replacement for the Konami AVS libraries (libacio, libbms,
 * libtouchpanel, libavs-win32).
 *
 * The arcade cabinet talks to an I/O board over serial, a card reader, a
 * touch panel and the ALL.Net server.  None of that exists on a PC, so these
 * functions report a healthy but offline cabinet: the game runs in free play
 * with local inputs, no card and no network.
 */

#define _GNU_SOURCE
#include "win32.h"
#include "platform.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static int g_boot_done;
static uint32_t g_coins;     /* coins inserted, not yet consumed by the game */

/* The Konami boot machine polls the BMS/touch-panel stubs once per frame.
 * Log the first few calls of each (needs OTOMEDIUS_LOG>=2) so the state
 * machine can be followed from the outside without patching generated code. */
#define AVS_TRACE(name, rv)                                                   \
    do {                                                                      \
        static unsigned n_;                                                   \
        if (n_ < 8)                                                           \
            log_printf("avs: %s -> %d (call %u)", name, (int)(rv), n_ + 1);   \
        n_++;                                                                 \
    } while (0)

/* ---------------------------------------------------------------- libacio */
static void io_avs_boot(void)
{
    log_printf("avs: ac_io_avs_boot");
    g_boot_done = 1;
    RET32(0);
}

static void io_begin(void) { RET32(1); }   /* BOOL: the board came up */
static void io_end(void) { RET32(0); }
static void io_shutdown(void) { }
static void io_update(void) { RET32(0); }
static void io_begin_get_status(void) { RET32(0); }
static void io_end_get_status(void) { RET32(0); }

static void io_get_control_status_buffer(void)
{
    uint32_t buf = arg32(0);
    if (buf) {
        pad_state_t pad;
        platform_read_pad(&pad);
        /* Cabinet test hook: OTOMEDIUS_AUTOCOIN=1 pulses the coin line for one
         * frame every 2 s and OTOMEDIUS_AUTOSTART=1 pulses start, so a build
         * can be driven past the credit/attract screens without a coin mech. */
        static unsigned frame;
        static int autostart;
        const char *ac = getenv("OTOMEDIUS_AUTOCOIN");
        const char *as = getenv("OTOMEDIUS_AUTOSTART");
        frame++;
        if (ac && *ac != '0' && (frame % 120) == 60)
            pad.coin = 1;
        if (as && *as != '0') {
            /* press start only while credits are available: the game keeps
             * the credit count at [0x5c30a0]-style globals, so pulse it for a
             * single frame after the coin */
            autostart = (frame % 120) == 90;
            if (autostart)
                pad.start = 1;
        }
        /* PJEI status buffer (game copy at 0x5DC950), read by 0x4132C0 state 5:
         *   +0x04 dword: switches, ACTIVE HIGH, remapped by the game through the
         *                (raw mask, game bit) table at 0x5C30A0;
         *   +0x10 byte : system switches, remapped through the table at 0x5C30E8.
         * Bit names come from the test mode INPUT CHECK screen (0x456E40: labels
         * at 0x5CD67C, game-bit masks at 0x5CD69C / 0x5CD6B8):
         *   raw 0x40 UP, 0x20 DOWN, 0x10 LEFT, 0x08 RIGHT, 0x04 A BUTTON (power-up),
         *   0x02 B BUTTON (shot), 0x01 C BUTTON (bomb), 0x80 START;
         *   system raw 0x20 TEST (test mode entered at 0x45480F), 0x10 SERVICE
         *   (service credit, 0x40D8D0), 0x04 COIN MECH.
         * Coins are NOT a switch bit: the game takes them from
         * ac_io_pjei_current_coinstock (see io_coinstock).  Checked on Windows
         * with the original exe (native loader, 2026-11-21): every switch in the
         * INPUT CHECK screen, A/B/C roles in game. */
        memset(MEM + buf, 0, 0x14);
        uint32_t sw = 0;
        if (pad.up) sw |= 0x40u;
        if (pad.down) sw |= 0x20u;
        if (pad.left) sw |= 0x10u;
        if (pad.right) sw |= 0x08u;
        if (pad.b1) sw |= 0x02u;     /* B BUTTON: shot */
        if (pad.b2) sw |= 0x01u;     /* C BUTTON: bomb */
        if (pad.b3) sw |= 0x04u;     /* A BUTTON: power-up */
        if (pad.start) sw |= 0x80u;
        M32(buf + 4) = sw;
        *(uint8_t *)(MEM + buf + 0x10) = 0;   /* system switches: none mapped yet */
        static int coin_prev;
        if (pad.coin && !coin_prev)
            g_coins++;
        coin_prev = pad.coin;
        if (g_verbose > 1)
            AVS_TRACE("ac_io_pjei_get_control_status_buffer", (int)sw);
    }
    RET32(0);
}

/* ac_io_pjei_update_control_status_buffer: must return non-zero, otherwise
 * 0x413552 takes the "no data" branch and the game never calls
 * ac_io_pjei_get_control_status_buffer - i.e. the cabinet input is frozen for
 * the whole run (this is why the coin/start pulses did nothing). */
static void io_update_control_buffer(void)
{
    RET32(1);
}

/* `ac_io_is_active2(int, int *status)` is only used by the cabinet-detection
 * state machine at 0x4132C0: a non-zero return means "board alive, go on and
 * query the security plug". */
static void io_is_active2(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0;
    RET32(1);
}
/* ac_io_pjei_current_coinstock(int slot, int *count) / consume_coinstock(int slot,
 * int count): 0x41356E reads the count into a local (arg 1, not arg 0) and, when it
 * is non-zero, inserts that many coins (0x40D3F0) and consumes them.  The pointer
 * used to be taken from arg 0 (the slot, 0), so the local was never written and the
 * game read an uninitialised stack value. */
static void io_coinstock(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = g_coins;
    RET32(1);
}
static void io_consume(void)
{
    uint32_t n = arg32(1);
    g_coins = n >= g_coins ? 0 : g_coins - n;
    RET32(1);
}
/* The security-plug handshake: request, then `_isfinished(&status)` with
 * status == 20 (0x14).  That is the value the detector at 0x4133D6 compares
 * against; anything else is treated as "illegal plug" and drops I/O mode to 7
 * (hard error), which is what used to wedge the whole boot. */
static void io_secplug_check(void) { RET32(1); }   /* request queued */
static void io_secplug_check_fin(void)
{
    uint32_t p = arg32(0);
    if (p)
        M32(p) = 0x14;
    RET32(1);
}
static void io_secplug_missing(void) { RET32(0); }
static void io_secplug_missing_fin(void) { RET32(0); }
/* Card reader (ICCA): request init, report "finished" with status 4, which the
 * detector at 0x41398C accepts as "card unit ready". */
static void io_cardunit_init(void) { RET32(1); }
static void io_cardunit_init_fin(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 4;
    RET32(1);
}
static void io_req_uid(void) { RET32(1); }
/* ac_io_icca_req_uid_isfinished / ac_io_icca_get_uid and the card service
 * live in card.c (virtual e-Amusement card, OTOMEDIUS_CARD). */
void card_icca_req_uid_fin(void);
void card_icca_get_uid(void);
void card_bms_inquire_wait(void);
void card_bms_wait_ok(void);
void card_bms_wait_register(void);
void card_bms_save(void);
void card_bms_load(void);
void card_bms_data_wait(void);
static void io_fw_dev_index(void) { RET32(0); }
static void io_watchdog(void) { RET32(0); }
static void io_go_fw_update(void) { RET32(0); }
static void io_lamp_bright(void) { RET32(0); }
static void io_coin_blocker_on(void) { RET32(0); }
static void io_coin_blocker_off(void) { RET32(0); }
static void io_lamp_mode(void) { RET32(0); }
static void io_lock_counter(void) { RET32(0); }
/* 0x413D3F treats 0 as a hard error: it logs "1183
 * ac_io_pjei_unlock_coincounter" through 0x403DB0, which sets the manager
 * enable mask (0x5DE220) to 0xFFFFFFFF and suspends every manager.  The
 * counter unlock is a cabinet bookkeeping call; report success. */
static void io_unlock_counter(void) { RET32(1); }
/* Both IDs must come back non-zero: 0x41341C/0x413446 give up (I/O mode 7) if
 * the getters fail.  The literal strings are the defaults already present in
 * the game's .data at 0x5BCA6C / 0x5BCAA0. */
static void io_systemid(void)
{
    uint32_t p = arg32(0);
    if (p)
        memcpy(MEM + p, "0123456789ABCDEFGHIJ", 20);
    RET32(1);
}
static void io_softwareid(void)
{
    uint32_t p = arg32(0);
    if (p)
        memcpy(MEM + p, "0123456789ABCDEFGHIJ", 20);
    RET32(1);
}

/* --------------------------------------------------------- libtouchpanel */
static void tp_new(void)
{
    /* the game keeps this handle and passes it back: give it real memory */
    extern uint32_t emu_alloc(uint32_t size);
    static uint32_t handle;
    if (!handle)
        handle = emu_alloc(64);
    RET32(handle);
}
static void tp_delete(void) { RET32(0); }
static void tp_polling(void) { AVS_TRACE("ac_touchpanel_polling", 0); RET32(0); }

/* The cabinet panel is 1280x1024 (same as the game window).  These report the
 * pen/mouse position as raw panel coordinates; with no pen or mouse activity
 * they stay zero, which is exactly what the previous stubs returned, so the
 * boot path is unaffected. */
#define PANEL_W 1280.0f
#define PANEL_H 1024.0f

static void tp_report(uint32_t xp, uint32_t yp, uint32_t zp, uint32_t pressp)
{
    touch_state_t t;
    platform_read_touch(&t);
    if (xp)
        M32(xp) = (uint32_t)(t.x * PANEL_W);
    if (yp)
        M32(yp) = (uint32_t)(t.y * PANEL_H);
    if (zp)
        M32(zp) = t.pressed ? (uint32_t)(t.pressure ? t.pressure : 1) : 0;
    if (pressp)
        M32(pressp) = t.pressed ? 1u : 0u;
}

static void tp_read_on(void)
{
    /* ac_touchpanel_read_on(handle, &x, &y, &pressure, &pressed) */
    tp_report(arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(1);   /* the panel answered */
}
static void tp_read_on_direct(void)
{
    tp_report(arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(1);
}
static void tp_check_recv(void) { RET32(0); }   /* reply not there yet */
static void tp_check_diag(void) { RET32(1); }   /* request sent */
/* ac_touchpanel_receive_check_diag(handle, &status): the status byte is tested
 * with `cmp byte [status],0` at 0x41372D, so it has to be non-zero. */
static void tp_recv_diag(void)
{
    uint32_t p = arg32(1);
    if (p)
        M8(p) = 1;
    RET32(1);   /* reply received, status OK */
}
static void tp_keepalive_on(void) { RET32(0); }
static void tp_keepalive_off(void) { RET32(0); }
static void tp_keepalive_err(void) { RET32(0); }
static void tp_calibrate(void) { RET32(0); }
static void tp_renewal(void) { RET32(0); }

/* --------------------------------------------------------------- libbms */
static void bms_is_active(void) { AVS_TRACE("bms_is_active", 1); RET32(1); }   /* the BMS layer is up */
static void bms_init_error(void) { RET32(0); }
static void bms_get_last_error(void) { RET32(0); }
static void bms_init(void) { AVS_TRACE("bms_init", 0); RET32(0); }
static void bms_init_wait(void) { AVS_TRACE("bms_init_wait", 0); RET32(0); }
static void bms_eventlog_init(void) { AVS_TRACE("bms_eventlog_request_init", 0); RET32(0); }
static void bms_ok(void) { RET32(0); }
/* bms_ea3_get_status_wait(int *status, int *sub) - the e-AMUSE client monitor.
 * The network-check state machine at 0x4296d0 calls it every few frames and
 * branches on the *result*: 0 = success (it then reads status), 1 = error,
 * >=2 = still working (it sleeps and retries).  The real DLL returns 1 until
 * ea3_boot() ran and then reports its connection state in *status.
 * The game's jump table at 0x429848 is {0,0,1,2} -> {0x429757,0x429757,
 * 0x42977d,0x4297ca}, so the status value indexes straight into it:
 *   0/1 -> 0x429757 -> [0x5de160] = 3   (not available / no connection)
 *   2   -> 0x42977d -> [0x5de160] = 2|4 (server message pending)
 *   3   -> 0x4297ca -> [0x5de160] = 5   (check finished, offline cabinet)
 * (the generated NETDBG label "case2" for 0x4297ca refers to the table slot,
 * not the status value).  [0x5de160] = 5 is the "network check done" state;
 * while [0x5de160] < 5 the 2D layer (0x455bc0) draws the red
 * "ネットワーク接続状況確認中" banner from MenuCmn's texture atlas.  Returning 0
 * from this stub without writing *status left the machine in status 0 forever,
 * so the banner never went away.  Report the offline-cabinet final state
 * (status 3): the check finished, there is no server message, no license
 * prompt.
 * The argument really is the address of the status int (the machine does
 * `lea esi,[ebx+0x10]` and later `mov esi,[esi]`), so write the value there,
 * not a pointer. */
static void bms_ea3_status_wait(void)
{
    uint32_t st = arg32(0), sub = arg32(1);
    AVS_TRACE("bms_ea3_get_status_wait", 0);
    if (getenv("OTOMEDIUS_NETDBG"))
        fprintf(stderr, "netdbg: get_status_wait st=%08x sub=%08x old=%u/%u ret=%08x "
                "ebx=%08x esi=%08x ecx=%08x g160=%u g164=%u\n",
                st, sub, st ? M32(st) : 0, sub ? M32(sub) : 0,
                M32(x86_cpu()->esp), x86_cpu()->ebx, x86_cpu()->esi,
                x86_cpu()->ecx, M16(0x5de160u), M16(0x5de164u));
    if (st)
        M32(st) = 3;   /* check finished: the game's table maps 3 -> [0x5de160]=5 */
    if (sub)
        M32(sub) = 0;  /* no sub-status (license / server message) */
    RET32(0);
}
/* The boot/attract manager (0x44D400) reads this 64-bit clock and divides a
 * delta by a frequency.  Returning a constant 0 made the divisor zero and the
 * game raised a divide-by-zero exception, which the CRT turned into an
 * unhandled exception before the Konami logo could be shown.  Report a real
 * monotonic millisecond counter instead. */
static void bms_time_clock(void)
{
    /* bms_time_get_clock_blocking(int type, uint64_t *out): the out pointer is
     * the *second* argument.  Reading arg0 as the pointer wrote to address 0/1
     * and left the caller's frequency at zero, which made 0x44D400 divide by
     * zero. */
    uint32_t p = arg32(1);
    struct timespec ts;
    uint64_t ms = 0;
    /* wall-clock ms: the game adds the signed offset stored in testmode.bin
     * (0x5DB710) and feeds the sum to ctime_s, so a small monotonic value
     * becomes a negative time64 and the CRT rejects it. */
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (p) {
        M32(p) = (uint32_t)(ms & 0xFFFFFFFFu);
        M32(p + 4) = (uint32_t)(ms >> 32);
    }
    RET32(0);
}

const import_def_t avs_imports[] = {
    {"libacio.dll", "ac_io_avs_boot", io_avs_boot, 0},
    {"libacio.dll", "ac_io_avs_shutdown", io_shutdown, 0},
    {"libacio.dll", "ac_io_begin", io_begin, 0},
    {"libacio.dll", "ac_io_end", io_end, 0},
    {"libacio.dll", "ac_io_update", io_update, 0},
    {"libacio.dll", "ac_io_begin_get_status", io_begin_get_status, 0},
    {"libacio.dll", "ac_io_end_get_status", io_end_get_status, 0},
    {"libacio.dll", "ac_io_pjei_get_control_status_buffer", io_get_control_status_buffer, 0},
    {"libacio.dll", "ac_io_pjei_update_control_status_buffer", io_update_control_buffer, 0},
    {"libacio.dll", "ac_io_is_active2", io_is_active2, 0},
    {"libacio.dll", "ac_io_pjei_current_coinstock", io_coinstock, 0},
    {"libacio.dll", "ac_io_pjei_consume_coinstock", io_consume, 0},
    {"libacio.dll", "ac_io_pjei_req_secplug_check", io_secplug_check, 0},
    {"libacio.dll", "ac_io_pjei_req_secplug_check_isfinished", io_secplug_check_fin, 0},
    {"libacio.dll", "ac_io_pjei_req_secplug_missing_check", io_secplug_missing, 0},
    {"libacio.dll", "ac_io_pjei_req_secplug_missing_check_isfinished", io_secplug_missing_fin, 0},
    {"libacio.dll", "ac_io_icca_cardunit_init", io_cardunit_init, 0},
    {"libacio.dll", "ac_io_icca_cardunit_init_isfinished", io_cardunit_init_fin, 0},
    {"libacio.dll", "ac_io_icca_req_uid", io_req_uid, 0},
    {"libacio.dll", "ac_io_icca_req_uid_isfinished", card_icca_req_uid_fin, 0},
    {"libacio.dll", "ac_io_icca_get_uid", card_icca_get_uid, 0},
    {"libacio.dll", "ac_io_get_firmware_update_device_index", io_fw_dev_index, 0},
    {"libacio.dll", "ac_io_pjei_get_watchdog_status", io_watchdog, 0},
    {"libacio.dll", "ac_io_go_firmware_update", io_go_fw_update, 0},
    {"libacio.dll", "ac_io_pjei_control_lamp_bright", io_lamp_bright, 0},
    {"libacio.dll", "ac_io_pjei_control_lamp_mode", io_lamp_mode, 0},
    {"libacio.dll", "ac_io_pjei_control_coin_blocker_on", io_coin_blocker_on, 0},
    {"libacio.dll", "ac_io_pjei_control_coin_blocker_off", io_coin_blocker_off, 0},
    {"libacio.dll", "ac_io_pjei_lock_coincounter", io_lock_counter, 0},
    {"libacio.dll", "ac_io_pjei_unlock_coincounter", io_unlock_counter, 0},
    {"libacio.dll", "ac_io_pjei_get_systemid", io_systemid, 0},
    {"libacio.dll", "ac_io_pjei_get_softwareid", io_softwareid, 0},

    {"libtouchpanel.dll", "ac_touchpanel_new", tp_new, 0},
    {"libtouchpanel.dll", "ac_touchpanel_delete", tp_delete, 0},
    {"libtouchpanel.dll", "ac_touchpanel_polling", tp_polling, 0},
    {"libtouchpanel.dll", "ac_touchpanel_read_on", tp_read_on, 0},
    {"libtouchpanel.dll", "ac_touchpanel_read_on_direct", tp_read_on_direct, 0},
    {"libtouchpanel.dll", "ac_touchpanel_check_wait_receive", tp_check_recv, 0},
    {"libtouchpanel.dll", "ac_touchpanel_send_check_diag", tp_check_diag, 0},
    {"libtouchpanel.dll", "ac_touchpanel_receive_check_diag", tp_recv_diag, 0},
    {"libtouchpanel.dll", "ac_touchpanel_set_keepalive_mode_on", tp_keepalive_on, 0},
    {"libtouchpanel.dll", "ac_touchpanel_set_keepalive_mode_off", tp_keepalive_off, 0},
    {"libtouchpanel.dll", "ac_touchpanel_get_keepalive_error", tp_keepalive_err, 0},
    {"libtouchpanel.dll", "ac_touchpanel_caribrate_set", tp_calibrate, 0},
    {"libtouchpanel.dll", "ac_touchpanel_renewal", tp_renewal, 0},

    {"libbms.dll", "bms_is_active", bms_is_active, 0},
    {"libbms.dll", "bms_init_error", bms_init_error, 0},
    {"libbms.dll", "bms_get_last_error", bms_get_last_error, 0},
    {"libbms.dll", "bms_init", bms_init, 0},
    {"libbms.dll", "bms_init_wait", bms_init_wait, 0},
    {"libbms.dll", "bms_time_get_clock_blocking", bms_time_clock, 0},
    {"libbms.dll", "bms_eventlog_request_init", bms_eventlog_init, 0},
    {"libbms.dll", "bms_eventlog_request_testmode", bms_ok, 0},
    {"libbms.dll", "bms_eventlog_request_otdemo", bms_ok, 0},
    {"libbms.dll", "bms_eventlog_request_transition", bms_ok, 0},
    {"libbms.dll", "bms_eventlog_request_game_start", bms_ok, 0},
    {"libbms.dll", "bms_eventlog_request_game_end", bms_ok, 0},
    {"libbms.dll", "bms_eventlog_request_eventdata", bms_ok, 0},
    {"libbms.dll", "bms_credit_update_option_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_reset_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_coin_throw_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_get_status_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_confirm_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_consume_blocking", bms_ok, 0},
    {"libbms.dll", "bms_credit_consume_expired", bms_ok, 0},
    {"libbms.dll", "bms_credit_service_blocking", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_inquire", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_inquire_wait", card_bms_inquire_wait, 0},
    {"libbms.dll", "bms_cardmng_cardactivate", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_cardactivate_wait", card_bms_wait_register, 0},
    {"libbms.dll", "bms_cardmng_bindcard", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindcard_wait", card_bms_wait_register, 0},
    {"libbms.dll", "bms_cardmng_bindmodel", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindmodel_wait", card_bms_wait_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogin", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogin_wait", card_bms_wait_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogout", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogout_wait", card_bms_wait_ok, 0},
    {"libbms.dll", "bms_playerdata_load", card_bms_load, 0},
    {"libbms.dll", "bms_playerdata_load_wait", card_bms_data_wait, 0},
    {"libbms.dll", "bms_playerdata_save", card_bms_save, 0},
    {"libbms.dll", "bms_playerdata_save_wait", card_bms_data_wait, 0},
    {"libbms.dll", "bms_ranking_get", bms_ok, 0},
    {"libbms.dll", "bms_ranking_get_wait", bms_ok, 0},
    {"libbms.dll", "bms_matching_query", bms_ok, 0},
    {"libbms.dll", "bms_matching_query_wait", bms_ok, 0},
    {"libbms.dll", "bms_matching_request", bms_ok, 0},
    {"libbms.dll", "bms_matching_request_wait", bms_ok, 0},
    {"libbms.dll", "bms_matching_reset_wait", bms_ok, 0},
    {"libbms.dll", "bms_get_server_message", bms_ok, 0},
    {"libbms.dll", "bms_get_server_message_wait", bms_ok, 0},
    {"libbms.dll", "bms_net_property_get", bms_ok, 0},
    {"libbms.dll", "bms_net_property_get_wait", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_initialize", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_server_accept", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_server_listen", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_server_close_dstsocket_all", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_sendto", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_recvfrom", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_client_connect_request", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_client_connect_wait", bms_ok, 0},
    {"libbms.dll", "bms_net_tcp_client_disconnect", bms_ok, 0},
    {"libbms.dll", "bms_keepalive_test_request", bms_ok, 0},
    {"libbms.dll", "bms_keepalive_test_request_wait", bms_ok, 0},
    {"libbms.dll", "bms_ea3_license_agree", bms_ok, 0},
    {"libbms.dll", "bms_ea3_license_agree_wait", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_status", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_status_wait", bms_ea3_status_wait, 0},
    {"libbms.dll", "bms_ea3_get_url_eapass", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_url_konaminetdx", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_eapass_valid", bms_ok, 0},
    {NULL, NULL, NULL, 0},
};
