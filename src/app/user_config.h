#ifndef USER_CONFIG_H_
#define USER_CONFIG_H_

#include "app_user_config.h"
#include "arch_api.h"
#include "app_default_handlers.h"
#include "app_adv_data.h"
#include "co_bt.h"

#define USER_CFG_ADDRESS_MODE    APP_CFG_ADDR_PUB
#define USER_CFG_CNTL_PRIV_MODE  APP_CFG_CNTL_PRIV_MODE_NETWORK

/* The sleep mode this image boots with.
 *
 * Extended sleep, and it is the only setting here that matters for battery
 * life: ARCH_SLEEP_OFF holds the chip in active mode and costs about 700 uA
 * doing nothing, against single-figure microamps for a tag that sleeps
 * between advertisements.
 *
 * It did not work for a long time and the cause was not in the sleep
 * machinery at all - periph_init() was not powering the peripheral domain
 * back up on wake, so the stack's own wake-time XTAL16M trim polled a
 * register that always read zero and spun until the watchdog fired. The
 * comment in periph_init() is the one worth reading.
 *
 * Command 0x0023 still switches this at runtime from RAM. Worth keeping: it
 * is the only way to take sleep out of the picture on a tag with no probe
 * on it, and a power cycle undoes it.
 *
 * The RCX stays the low-power clock - this board has no 32 kHz crystal - and
 * the flash is powered down while idle either way, see
 * atc_idle_power_down().
 */
#define ATC_SLEEP_DEFAULT_ON 1

/* Diagnostic scaffolding: a cumulative power staircase, see probe_tick().
 * Set to 1 only to attribute a sleep-current floor with a meter attached -
 * a tag built with it stops advertising eight seconds after boot.
 */
#define ATC_POWER_PROBE 0

#if ATC_SLEEP_DEFAULT_ON
static const sleep_state_t app_default_sleep_mode = ARCH_EXT_SLEEP_ON;
#else
static const sleep_state_t app_default_sleep_mode = ARCH_SLEEP_OFF;
#endif

/* Both ends of the advertising interval are 3200 slots. The unit is
 * 0.625 ms, so 3200 is 2.000 s. Sysinfo reports the same number.
 * Extended sleep stays off. Command 0x0008 can change the next start.
 */
static const struct advertise_configuration user_adv_conf = {
    .addr_src = APP_CFG_ADDR_SRC(USER_CFG_ADDRESS_MODE),
    .intv_min = MS_TO_BLESLOTS(2000),
    .intv_max = MS_TO_BLESLOTS(2000),
    .channel_map = ADV_ALL_CHNLS_EN,
    .mode = GAP_GEN_DISCOVERABLE,
    .adv_filt_policy = ADV_ALLOW_SCAN_ANY_CON_ANY,
    .peer_addr = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
    .peer_addr_type = 0,
};

/* Placeholder for the version-2 manufacturer record, company id 0x1337.
 * user_advertise replaces it before the radio starts: hardware 0x0073,
 * firmware 0x0030, capabilities 0x01C3, then the live battery, temperature
 * and counter, plus the MAC name in the same packet. The stack adds the
 * flags. The device-name macro stays empty so a second name is rejected.
 * Scan response carries the 16-bit service UUID 0x1337.
 */
#define USER_ADVERTISE_DATA \
    "\x0E\xFF\x37\x13" \
    "\x02" \
    "\x73\x00" \
    "\x30\x00" \
    "\xC3\x01" \
    "\x00\x00" \
    "\x00" \
    "\x00"
#define USER_ADVERTISE_DATA_LEN     (sizeof(USER_ADVERTISE_DATA) - 1)
#define USER_ADVERTISE_SCAN_RESPONSE_DATA     "\x03\x03\x37\x13"
#define USER_ADVERTISE_SCAN_RESPONSE_DATA_LEN (sizeof(USER_ADVERTISE_SCAN_RESPONSE_DATA) - 1)

#define USER_DEVICE_NAME            ""
#define USER_DEVICE_NAME_LEN        (sizeof(USER_DEVICE_NAME) - 1)

static const struct gapm_configuration user_gapm_conf = {
    .role = GAP_ROLE_PERIPHERAL,
    /* A 235-byte write needs ATT MTU >= 238. 247 is the common request. */
    .max_mtu = 247,
    .addr_type = APP_CFG_ADDR_TYPE(USER_CFG_ADDRESS_MODE),
    .renew_dur = 15000,
    .addr = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    .irk = {0xA7, 0xC0, 0x11, 0x5E, 0x88, 0x04, 0x3D, 0x90,
            0x2B, 0x67, 0xE1, 0x44, 0x19, 0xC3, 0x70, 0x58},
    .att_cfg = GAPM_MASK_ATT_SVC_CHG_EN,
    .gap_start_hdl = 0,
    .gatt_start_hdl = 0,
    .max_mps = 251,
    .max_txoctets = 251,
    .max_txtime = 2120,
};

/* Idle link: 30-50 ms. Command 0x0002 asks for 7.5-15 ms and 0x0003 returns here.
 * 600 is 6 seconds, in units of 10 ms.
 */
static const struct connection_param_configuration user_connection_param_conf = {
    .intv_min = MS_TO_DOUBLESLOTS(30),
    .intv_max = MS_TO_DOUBLESLOTS(50),
    .latency = 0,
    .time_out = 600,
    .ce_len_min = 0,
    .ce_len_max = 0,
};

static const struct default_handlers_configuration user_default_hnd_conf = {
    .adv_scenario = DEF_ADV_FOREVER,
    .advertise_period = MS_TO_TIMERUNITS(180000),
    .security_request_scenario = DEF_SEC_REQ_NEVER,
};

static const struct central_configuration user_central_conf = {
    .code = GAPM_CONNECTION_DIRECT,
    .addr_src = APP_CFG_ADDR_SRC(USER_CFG_ADDRESS_MODE),
    .scan_interval = 0x180,
    .scan_window = 0x160,
    .con_intv_min = 100,
    .con_intv_max = 100,
    .con_latency = 0,
    .superv_to = 0x1F4,
    .ce_len_min = 0,
    .ce_len_max = 0x5,
    .peer_addr_0 = {0, 0, 0, 0, 0, 0},
    .peer_addr_0_type = 0,
    .peer_addr_1 = {0, 0, 0, 0, 0, 0},
    .peer_addr_1_type = 0,
    .peer_addr_2 = {0, 0, 0, 0, 0, 0},
    .peer_addr_2_type = 0,
    .peer_addr_3 = {0, 0, 0, 0, 0, 0},
    .peer_addr_3_type = 0,
    .peer_addr_4 = {0, 0, 0, 0, 0, 0},
    .peer_addr_4_type = 0,
    .peer_addr_5 = {0, 0, 0, 0, 0, 0},
    .peer_addr_5_type = 0,
    .peer_addr_6 = {0, 0, 0, 0, 0, 0},
    .peer_addr_6_type = 0,
    .peer_addr_7 = {0, 0, 0, 0, 0, 0},
    .peer_addr_7_type = 0,
};

static const struct security_configuration user_security_conf = {
    .iocap = GAP_IO_CAP_NO_INPUT_NO_OUTPUT,
    .oob = GAP_OOB_AUTH_DATA_NOT_PRESENT,
    .auth = GAP_AUTH_NONE,
    .key_size = KEY_LEN,
    .ikey_dist = GAP_KDIST_NONE,
    .rkey_dist = GAP_KDIST_ENCKEY,
    .sec_req = GAP_NO_SEC,
};

#endif
