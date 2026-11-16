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

static int g_boot_done;

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
        memset(MEM + buf, 0, 16);
        /* PJEI cabinet: 16 bits of switch inputs, active low */
        uint16_t sw = 0xFFFF;
        if (pad.up) sw &= ~0x0001u;
        if (pad.down) sw &= ~0x0002u;
        if (pad.left) sw &= ~0x0004u;
        if (pad.right) sw &= ~0x0008u;
        if (pad.b1) sw &= ~0x0010u;
        if (pad.b2) sw &= ~0x0020u;
        if (pad.b3) sw &= ~0x0040u;
        if (pad.b4) sw &= ~0x0080u;
        if (pad.start) sw &= ~0x0100u;
        if (pad.coin) sw &= ~0x0200u;
        M16(buf) = sw;
    }
    RET32(0);
}

static void io_update_control_buffer(void)
{
    RET32(0);
}

static void io_is_active2(void) { RET32(0); }   /* 0 = the board is ready */
static void io_coinstock(void)
{
    uint32_t p = arg32(0);
    if (p)
        M32(p) = 0;
    RET32(0);
}
static void io_consume(void) { RET32(0); }
static void io_secplug_check(void) { RET32(0); }   /* 0 = security plug present */
static void io_secplug_check_fin(void) { RET32(1); }
static void io_secplug_missing(void) { RET32(0); }
static void io_secplug_missing_fin(void) { RET32(0); }
static void io_cardunit_init(void) { RET32(0); }
static void io_cardunit_init_fin(void) { RET32(1); }
static void io_req_uid(void) { RET32(1); }
static void io_req_uid_fin(void) { RET32(1); }
static void io_get_uid(void) { RET32(0); }
static void io_fw_dev_index(void) { RET32(0); }
static void io_watchdog(void) { RET32(0); }
static void io_go_fw_update(void) { RET32(0); }
static void io_lamp_bright(void) { RET32(0); }
static void io_coin_blocker_on(void) { RET32(0); }
static void io_coin_blocker_off(void) { RET32(0); }
static void io_lamp_mode(void) { RET32(0); }
static void io_lock_counter(void) { RET32(0); }
static void io_unlock_counter(void) { RET32(0); }
static void io_systemid(void)
{
    uint32_t p = arg32(0);
    if (p) {
        memcpy(MEM + p, "0123456789ABCDEF", 16);
    }
    RET32(0);
}
static void io_softwareid(void)
{
    uint32_t p = arg32(0);
    if (p)
        memcpy(MEM + p, "0000000000000000", 16);
    RET32(0);
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
static void tp_polling(void) { RET32(0); }
static void tp_read_on(void) { RET32(0); }
static void tp_read_on_direct(void) { RET32(0); }
static void tp_check_recv(void) { RET32(0); }
static void tp_check_diag(void) { RET32(1); }   /* request sent */
static void tp_recv_diag(void) { RET32(0); }   /* reply received, status OK */
static void tp_keepalive_on(void) { RET32(0); }
static void tp_keepalive_off(void) { RET32(0); }
static void tp_keepalive_err(void) { RET32(0); }
static void tp_calibrate(void) { RET32(0); }
static void tp_renewal(void) { RET32(0); }

/* --------------------------------------------------------------- libbms */
static void bms_is_active(void) { RET32(1); }   /* the BMS layer is up */
static void bms_init_error(void) { RET32(0); }
static void bms_get_last_error(void) { RET32(0); }
static void bms_init(void) { RET32(0); }
static void bms_init_wait(void) { RET32(0); }
static void bms_ok(void) { RET32(0); }
static void bms_time_clock(void)
{
    uint32_t p = arg32(0);
    if (p)
        M32(p) = (uint32_t)(0);
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
    {"libacio.dll", "ac_io_icca_req_uid_isfinished", io_req_uid_fin, 0},
    {"libacio.dll", "ac_io_icca_get_uid", io_get_uid, 0},
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
    {"libbms.dll", "bms_eventlog_request_init", bms_ok, 0},
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
    {"libbms.dll", "bms_cardmng_inquire_wait", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_cardactivate", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_cardactivate_wait", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindcard", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindcard_wait", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindmodel", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_bindmodel_wait", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogin", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogin_wait", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogout", bms_ok, 0},
    {"libbms.dll", "bms_cardmng_playerlogout_wait", bms_ok, 0},
    {"libbms.dll", "bms_playerdata_load", bms_ok, 0},
    {"libbms.dll", "bms_playerdata_load_wait", bms_ok, 0},
    {"libbms.dll", "bms_playerdata_save", bms_ok, 0},
    {"libbms.dll", "bms_playerdata_save_wait", bms_ok, 0},
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
    {"libbms.dll", "bms_ea3_get_status_wait", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_url_eapass", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_url_konaminetdx", bms_ok, 0},
    {"libbms.dll", "bms_ea3_get_eapass_valid", bms_ok, 0},
    {NULL, NULL, NULL, 0},
};
