#include "user_atc.h"

#include "adc.h"
#include "app_api.h"
#include "app_easy_timer.h"
#include "arch.h"
#include "arch_api.h"
#include "arch_wdg.h"
#include "custs1_task.h"
#include "epd.h"
#include "gui.h"
#include "gapc_task.h"
#include "gattc_task.h"
#include "ke_msg.h"
#include "oepl_session.h"
#include "panel.h"
#include "prf_types.h"
#include "prf_utils.h"
#include "rtt.h"
#include "settings.h"
#include "spi_58x.h"
#include "spi_flash.h"
#include "user_periph_setup.h"

#include <string.h>

/* Received pictures live past the product header and the waveform.
 * A firmware file is staged here, then copied into the application bank.
 * The boot image, pin record, geometry, waveform and product header stay put.
 */
#define ATC_IMAGE_BASE 0x00060000u
#define ATC_FLASH_END  0x00080000u
/* Past the two image planes. A stored picture writes this after a refresh.
 * An upload or a firmware file erases the 64 KB block, so the next boot
 * draws the status screen again.
 */
#define ATC_MARK_ADDR  0x0007F000u
#define ATC_BANK_BASE  0x0001F000u
#define ATC_BANK_END   0x00038000u
#define ATC_SECTOR     4096u
/* Own 4 KiB sector. Picture erase covers 0x60000 and 0x70000,
 * so an upload leaves this record. 0x78000 sits inside the picture window.
 */
#define ATC_NDEF_ADDR  0x00050000u
/* The settings record, in the next sector along. Its own sector because 4 KiB
 * is the smallest thing this flash erases, and sharing one with the NDEF
 * message would mean rewriting a message to change an address. Nothing else
 * touches it: picture erase covers 0x60000 and 0x70000, a firmware copy
 * covers the bank at 0x1F000.
 */
#define ATC_CFG_ADDR   0x00051000u

/* 16-bit UUID 0x1337 on the Bluetooth base. The uploader asks for this
 * UUID as the primary service and again as the characteristic.
 */
static const uint8_t atc_uuid_1337[ATT_UUID_128_LEN] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x37, 0x13, 0x00, 0x00,
};

static const uint16_t att_decl_svc = ATT_DECL_PRIMARY_SERVICE;
static const uint16_t att_decl_char = ATT_DECL_CHARACTERISTIC;
static const uint16_t att_desc_cfg = ATT_DESC_CLIENT_CHAR_CFG;

struct attm_desc_128 custs1_att_db[ATC_IDX_NB] = {
    {(uint8_t *)&att_decl_svc, ATT_UUID_128_LEN, PERM(RD, ENABLE),
     sizeof atc_uuid_1337, sizeof atc_uuid_1337, (uint8_t *)atc_uuid_1337},
    {(uint8_t *)&att_decl_char, ATT_UUID_16_LEN, PERM(RD, ENABLE), 0, 0, NULL},
    /* WRITE_REQ is the Write Request (with response). The access point sends
     * every 235-byte block part that way. WRITE_COMMAND alone accepts only a
     * Write Command, and the stack answers a Write Request with 0x06.
     * RI keeps the value out of the attribute database: the app consumes it.
     */
    {(uint8_t *)atc_uuid_1337, ATT_UUID_128_LEN,
     PERM(WR, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE) | PERM(NTF, ENABLE),
     PERM(RI, ENABLE) | ATC_OEPL_MAX_LEN, 0, NULL},
    {(uint8_t *)&att_desc_cfg, ATT_UUID_16_LEN,
     PERM(RD, ENABLE) | PERM(WR, ENABLE) | PERM(WRITE_REQ, ENABLE),
     2, 0, NULL},
};

const uint8_t custs1_services[] = {ATC_IDX_SVC, ATC_IDX_NB};
const uint8_t custs1_services_size = 1;
const uint16_t custs1_att_max_nb = ATC_IDX_NB;

/* Capabilities reported in advertising and in GET_INFOS. */
#define ATC_CAPABILITIES  0x01C3u
/* Little-endian on the air: bytes 0x30, 0x00. Decimal 48. The access point
 * turns on zlib images once a tag reports a version of 0x27 or higher.
 */
#define ATC_FW_VERSION    0x0030u

extern struct bd_addr dev_bdaddr;

static oepl_session atc_session;
static uint8_t atc_conidx;
static uint8_t atc_connected;
static uint8_t atc_counter;
static timer_hnd atc_adv_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t flash_open;
static uint8_t refresh_pending;
static uint8_t refresh_active;
static uint8_t refresh_type;
/* 0 is a stored picture. 1 is the status screen. */
static uint8_t refresh_source;
static uint8_t image_kept;
static uint8_t boot_done;
/* Picture area blanked for this version, in 4 KiB steps, before block 0. */
static uint8_t image_blank_ok;
static uint8_t image_erase_on;
static uint16_t image_erase_at;
static uint64_t image_blank_ver;
static uint64_t image_erase_ver;
static timer_hnd image_erase_timer = EASY_TIMER_INVALID_TIMER;
static timer_hnd zlib_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t image_hold[32];
static uint16_t image_hold_len;
static uint8_t image_hold_on;
/* Sector erase was started and the status register is still busy. */
static uint8_t image_sector_wait;
/* Firmware staging erase is running for the block already held in the session. */
static uint8_t staging_hold;
/* The current firmware file has started its staging erase. */
static uint8_t fw_staged;
static timer_hnd store_timer = EASY_TIMER_INVALID_TIMER;
static void image_erase_arm(uint16_t delay);
static void image_erase_tick(void);
static void store_arm(uint16_t delay);
static const uint8_t image_mark[8] = {'A', 'T', 'C', 'I', 'M', 'G', 1, 0};
static uint8_t led_color;
static uint8_t temp_tries;
static uint8_t temp_valid;
static uint8_t sense_armed;
static uint8_t ota_busy;
static uint8_t ndef_busy;
static uint8_t ndef_erase_wait;
static timer_hnd ota_timer = EASY_TIMER_INVALID_TIMER;
static timer_hnd ndef_timer = EASY_TIMER_INVALID_TIMER;
/* 1 while undirected advertising has been started and not completed. */
static uint8_t adv_on;
/* 0 unknown, 1 slow (30-50 ms), 2 fast (7.5-15 ms). */
static uint8_t link_mode;
static uint8_t link_want;
static uint8_t link_inflight;
static uint8_t link_pending;
static timer_hnd power_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t power_action;

/* Command 1 is the lamp. 0 is off, 1 red, 2 green, 3 blue, 4 red+green,
 * 5 red+blue, 6 green+blue, 7 every pad. A missing color byte lights every
 * pad.
 * Command 0x0015 is the 12-byte blink pattern.
 */
#define ATC_CMD_LED      0x0001u
#define ATC_CMD_LEDFLASH 0x0015u
/* Both are stored and then rebooted into. See the settings record. */
#define ATC_CMD_SETTYPE  0x0004u
#define ATC_CMD_SETMAC   0x0009u
/* Advertising interval, big-endian, in 0.625 ms slots. 100..10000, else 3200. */
#define ATC_CMD_PERIOD 0x0008u
#define ATC_CMD_DEEP   0x0014u
#define ATC_CMD_REBOOT 0x0017u
/* This build's own, see OEPL_CMD_SLEEP: 0 off, 1 extended sleep. RAM only. */
#define ATC_CMD_SLEEP  0x0023u
#define ATC_ADV_SLOTS_MIN 100u
#define ATC_ADV_SLOTS_MAX 10000u
#define ATC_ADV_SLOTS_DEF 3200u
/* Interval units are 1.25 ms. 6 is the BLE minimum, 7.5 ms. */
#define ATC_FAST_MIN   6u
#define ATC_FAST_MAX   12u
#define ATC_SLOW_MIN   24u
#define ATC_SLOW_MAX   40u
#define ATC_LINK_TIMEOUT 600u

/* Advertising ticks between battery samples. The tick is 2 s, so 30 of them
 * is a minute. See atc_adv_tick().
 */
/* Seconds between battery and temperature passes. */
#define ATC_SENSE_SECONDS 60u

/* The idle tick, in 10 ms units. 2 s while there is work to pump, 10 s when
 * the tag is only advertising - see atc_tick_delay().
 */
#define ATC_TICK_BUSY 200u
#define ATC_TICK_IDLE 1000u

/* How long after boot the SWD pads stay alive, in seconds.
 *
 * P1_4 and P1_5 belong to the debug module from reset, whether a probe is
 * attached or not, and the firmware never touched them. Handing them back and
 * leaving them floating is worth 122 uA - the single largest consumer on this
 * tag, and invisible from inside the firmware, which is why it sat there for
 * so long.
 *
 * That number comes from toggling it inside one boot rather than comparing
 * two builds: 140.7 uA with the pads as the debug module leaves them, 18.5 uA
 * one phase later with them released, and the panel's own 70 uA already dealt
 * with before either reading. Comparing separate builds had me attribute this
 * to the panel and then to nothing at all, in that order.
 *
 * A grace period rather than an outright release, so the tag can still be
 * attached to: for this long after every power-up, SWD works as usual. After
 * it, flashing needs a power cycle first - which is the same trade the SDK
 * makes for deep sleep, where it disables the debugger outright before going
 * down (arch_sleep.c).
 */
#define ATC_SWD_GRACE_SECONDS 30u


/* ---- easy timers, and why they are counted -------------------------------
 *
 * The SDK has exactly ten of them. APP_TIMER_MAX_NUM is
 * APP_TIMER_API_MES9 - APP_TIMER_API_MES0 + 1, the slots are a fixed table,
 * and atc_timer() returns EASY_TIMER_INVALID_TIMER when they are all
 * taken. It does not complain; the caller simply gets no timer and whatever
 * it scheduled never happens.
 *
 * This firmware asks for more than ten. During one image upload it can want
 * the advertising tick, the stall guard, the picture erase, the block store,
 * the zlib slice, the refresh chain, the log pump, a lamp pattern, the
 * sensor pass and a block-request retry, and a firmware update adds another.
 * Which one loses is down to timing, and that is exactly the shape of a
 * fault that shows up "every now and then".
 *
 * The worst loser is the advertising tick, because it re-arms itself and is
 * the only thing that calls user_advertise(): lose it once and the tag stops
 * advertising for good, which from the outside looks like the tag crashing
 * mid-upload. So every arm goes through here, failures are counted, and the
 * count is logged - a silent failure becomes a visible one.
 */
static uint16_t timer_fails;
static uint16_t block_bad_said;
static uint16_t sense_countdown;   /* seconds until the next sensor pass */
static uint8_t sleep_mode_on;
static uint16_t sleep_woke;
static uint16_t sleep_woke_said;

#if ATC_POWER_PROBE
/* Set once the probe has stopped the radio. Without it the stop is undone
 * immediately: app_easy_gap_advertise_stop() ends in user_on_adv_complete(),
 * which arms atc_adv_tick(), which calls user_advertise() again.
 */
static uint8_t probe_quiet;
#endif

static timer_hnd atc_timer(uint32_t delay, void (*fn)(void))
{
    timer_hnd h = app_easy_timer((uint16_t)delay, fn);

    if (h == EASY_TIMER_INVALID_TIMER) {
        if (timer_fails != 0xFFFFu) {
            timer_fails++;
        }
        rtt_puts("timer full n ");
        rtt_u32(timer_fails);
        rtt_puts("\n");
    }
    return h;
}

struct led_pad {
    GPIO_PORT port;
    GPIO_PIN pin;
    uint8_t mask;
};

/* Mask bit 0x80 marks a pad that only follows the "all" color. */
static const struct led_pad led_pads[] = {
    {LED_R_PORT, LED_R_PIN, 0x01},
    {LED_G_PORT, LED_G_PIN, 0x02},
    {LED_B_PORT, LED_B_PIN, 0x04},
    {LED_X_PORT, LED_X_PIN, 0x80},
    {LED_Y_PORT, LED_Y_PIN, 0x80},
    {LED_A_PORT, LED_A_PIN, 0x80},
    {LED_C_PORT, LED_C_PIN, 0x80},
    {LED_D_PORT, LED_D_PIN, 0x80},
    {LED_E_PORT, LED_E_PIN, 0x80},
    {LED_F_PORT, LED_F_PIN, 0x80},
    {LED_Z_PORT, LED_Z_PIN, 0x80},
};

/* all lights every pad. Otherwise only the red, green and blue masks.
 *
 * A board whose config does not carry ATC_CAP_LED has nothing fitted on
 * these pads, so they are left exactly as periph_init() set them. The LED
 * commands still acknowledge - a host should not have to know which board
 * it is talking to - they simply have nothing to light.
 */
static void led_drive(int all, uint8_t bits)
{
    unsigned i;

    if (!atc_panel_has(ATC_CAP_LED)) {
        return;
    }
    for (i = 0; i < (sizeof led_pads / sizeof led_pads[0]); i++) {
        int on = 0;

        if (all) {
            on = 1;
        } else if ((led_pads[i].mask & 0x80u) == 0u) {
            on = (bits & led_pads[i].mask) != 0u;
        }
        /* Common anode: a high pad is dark. */
        GPIO_ConfigurePin(led_pads[i].port, led_pads[i].pin, OUTPUT, PID_GPIO,
                          on ? false : true);
        /* And record it, so a wake from a long sleep replays this lamp and
         * not the one set_pad_functions() left behind.
         */
        atc_pad_level((unsigned)led_pads[i].port, (unsigned)led_pads[i].pin,
                      on ? 0 : 1);
    }
}

static void led_apply(void)
{
    /* 3 is blue. The mixes sit above the three primary values. */
    static const uint8_t mix[7] = {0x00, 0x01, 0x02, 0x04, 0x03, 0x05, 0x06};

    if (led_color >= 7u) {
        led_drive(1, 0);
    } else {
        led_drive(0, mix[led_color]);
    }
}

/* Blink pattern, little-endian nibbles as the access point packs ledFlash:
 * byte0 low mode, high on-time in ms (0 is 500 us, 15 holds the lamp on),
 * then three groups of color, count|speed, delay, then repeats.
 * Speed and each group delay are multiples of 100 ms.
 * repeats 0 plays the three groups once. 255 plays until stop or 0x0001.
 * A channel lights when its RGB332 part is in the upper half. A dimmer
 * non-zero color lights only its strongest channel.
 */
static uint8_t led_pat[12];
static uint8_t led_run;
static uint8_t led_forever;
static uint8_t led_loops;
static uint8_t led_group;
static uint8_t led_left;
static timer_hnd led_timer = EASY_TIMER_INVALID_TIMER;

static void led_advance(void);

static uint8_t led_rgb_bits(uint8_t color)
{
    uint8_t r = (uint8_t)((color >> 5) & 7u);
    uint8_t g = (uint8_t)((color >> 2) & 7u);
    uint8_t b = (uint8_t)(color & 3u);
    uint8_t bits = 0;

    if (r >= 4u) {
        bits = (uint8_t)(bits | 0x01u);
    }
    if (g >= 4u) {
        bits = (uint8_t)(bits | 0x02u);
    }
    if (b >= 2u) {
        bits = (uint8_t)(bits | 0x04u);
    }
    if (bits == 0u && color != 0u) {
        if (r >= g && r >= b) {
            bits = 0x01u;
        } else if (g >= b) {
            bits = 0x02u;
        } else {
            bits = 0x04u;
        }
    }
    return bits;
}

static void led_pulse(uint8_t rgb332)
{
    uint8_t dur = (uint8_t)(led_pat[0] >> 4);

    led_drive(0, led_rgb_bits(rgb332));
    if (dur >= 15u) {
        return;
    }
    arch_asm_delay_us(dur == 0u ? 500 : (int)dur * 1000);
    led_drive(0, 0);
}

static void led_pattern_stop(void)
{
    led_run = 0;
    led_left = 0;
    led_group = 0;
    if (led_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(led_timer);
        led_timer = EASY_TIMER_INVALID_TIMER;
    }
    led_apply();
}

static void led_arm(uint32_t delay)
{
    if (!led_run || led_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    if (delay == 0u) {
        delay = 1u;
    }
    led_timer = atc_timer(delay, led_advance);
    if (led_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("led timer\n");
    }
}

static uint8_t led_grp_count(uint8_t group)
{
    return (uint8_t)(led_pat[2u + (group * 3u)] & 0x0Fu);
}

static uint8_t led_grp_speed(uint8_t group)
{
    return (uint8_t)(led_pat[2u + (group * 3u)] >> 4);
}

static void led_advance(void)
{
    led_timer = EASY_TIMER_INVALID_TIMER;
    while (led_run) {
        uint16_t gap;

        if (led_group >= 3u) {
            if (led_forever) {
                led_group = 0;
                led_left = 0;
                continue;
            }
            if (led_loops > 1u) {
                led_loops = (uint8_t)(led_loops - 1u);
                led_group = 0;
                led_left = 0;
                continue;
            }
            rtt_puts("led done\n");
            led_pattern_stop();
            return;
        }
        if (led_left == 0u) {
            uint8_t count = led_grp_count(led_group);

            if (count == 0u) {
                led_group = (uint8_t)(led_group + 1u);
                continue;
            }
            led_left = count;
            gap = (uint16_t)led_grp_speed(led_group) * 10u;
            if (gap != 0u) {
                led_arm(gap);
                return;
            }
        }
        led_pulse(led_pat[1u + (led_group * 3u)]);
        led_left = (uint8_t)(led_left - 1u);
        if (led_left > 0u) {
            gap = (uint16_t)led_grp_speed(led_group) * 10u;
            if (gap != 0u) {
                led_arm(gap);
                return;
            }
            continue;
        }
        gap = (uint16_t)led_pat[3u + (led_group * 3u)] * 10u;
        led_group = (uint8_t)(led_group + 1u);
        if (gap != 0u) {
            led_arm(gap);
            return;
        }
    }
}

static void led_pattern_begin(const uint8_t *raw, uint16_t n)
{
    uint8_t mode;

    if (n > sizeof led_pat) {
        n = (uint16_t)sizeof led_pat;
    }
    memset(led_pat, 0, sizeof led_pat);
    if (n != 0u && raw != 0) {
        memcpy(led_pat, raw, n);
    }
    mode = (uint8_t)(led_pat[0] & 0x0Fu);
    if (led_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(led_timer);
        led_timer = EASY_TIMER_INVALID_TIMER;
    }
    led_run = 0;
    led_left = 0;
    led_group = 0;
    if (mode != 1u || ((led_pat[2] | led_pat[5] | led_pat[8]) & 0x0Fu) == 0u) {
        led_apply();
        return;
    }
    led_forever = (led_pat[10] == 255u) ? 1u : 0u;
    led_loops = led_pat[10] == 0u ? 1u : led_pat[10];
    led_run = 1;
    rtt_puts("led pat\n");
    led_advance();
}

static int flash_bringup(void)
{
    spi_cfg_t cfg;
    spi_flash_cfg_t env;
    uint32_t jedec = 0;
    int8_t status;

    if (flash_open) {
        return 0;
    }
    /* The SPI block lives in the peripheral power domain, and an idle tag now
     * puts that domain to sleep - see atc_idle_power_down(). Raise it again
     * and wait for it, or every register write below lands nowhere and the
     * JEDEC read comes back as zeros.
     */
    SetBits16(PMU_CTRL_REG, PERIPH_SLEEP, 0);
    {
        uint32_t spins = 100000u;

        while (spins-- != 0u && (GetWord16(SYS_STAT_REG) & PER_IS_UP) == 0u) {
        }
    }
    if ((GetWord16(SYS_STAT_REG) & PER_IS_UP) == 0u) {
        rtt_puts("periph down\n");
        return -1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.spi_ms = SPI_MS_MODE_MASTER;
    cfg.spi_cp = SPI_CP_MODE_0;
    cfg.spi_speed = SPI_SPEED_MODE_2MHz;
    cfg.spi_wsz = SPI_MODE_8BIT;
    cfg.spi_cs = SPI_CS_0;
    cfg.spi_irq = SPI_IRQ_DISABLED;
    cfg.cs_pad.port = FLASH_CS_PORT;
    cfg.cs_pad.pin = FLASH_CS_PIN;

    spi_initialize(&cfg);
    status = spi_flash_release_from_power_down();
    rtt_puts("flash rdp ");
    rtt_u32((uint32_t)(int8_t)status);
    rtt_puts("\n");
    status = spi_flash_read_jedec_id(&jedec);
    rtt_puts("flash id ");
    rtt_hex16((uint16_t)(jedec >> 16));
    rtt_hex16((uint16_t)jedec);
    rtt_puts(" st ");
    rtt_u32((uint32_t)(int8_t)status);
    rtt_puts("\n");
    if (status != SPI_FLASH_ERR_OK || jedec == 0 || jedec == 0xFFFFFFFFu) {
        return -1;
    }
    env.dev_index = AT25XE041B_DEV_INDEX;
    env.jedec_id = jedec;
    env.chip_size = SPI_FLASH_SIZE_4MBIT;
    spi_flash_configure_env(&env);
    status = spi_flash_configure_memory_protection(SPI_FLASH_MEM_PROT_NONE);
    rtt_puts("flash prot ");
    rtt_u32((uint32_t)(int8_t)status);
    rtt_puts("\n");
    flash_open = 1;
    return 0;
}

/* 0: the sector erase command was accepted. 1: the chip is still busy, so
 * the command was not sent. -1: the command was refused.
 * The status register is not polled here. A 4 KiB erase takes tens of
 * milliseconds, and waiting for it inside a BLE callback fills the heap.
 */
static int erase_kick(uint32_t address)
{
    int8_t status;

    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (flash_bringup() != 0) {
        return -1;
    }
    status = spi_flash_block_erase_no_wait(address, SPI_FLASH_OP_SE);
    if (status == SPI_FLASH_ERR_BUSY) {
        return 1;
    }
    if (status != SPI_FLASH_ERR_OK) {
        return -1;
    }
    return 0;
}

/* 0: still erasing. 1: ready. -1: the status read failed. */
static int erase_poll(void)
{
    int8_t status;

    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (flash_bringup() != 0) {
        return -1;
    }
    status = spi_flash_is_busy();
    if (status == SPI_FLASH_ERR_BUSY) {
        return 0;
    }
    if (status != SPI_FLASH_ERR_OK) {
        return -1;
    }
    return 1;
}

static void staging_erase_begin(void)
{
    if (image_erase_on && staging_hold && image_erase_ver == atc_session.xfer_ver) {
        return;
    }
    image_erase_ver = atc_session.xfer_ver;
    image_erase_at = 0;
    image_erase_on = 1;
    image_blank_ok = 0;
    image_hold_on = 0;
    staging_hold = 1;
    image_sector_wait = 0;
    rtt_puts("store erase\n");
    image_erase_arm(1);
}

/* The picture window is already blank. One 4096-byte block is sixteen page
 * programs, a few tens of milliseconds. Returning after every 512 bytes put
 * a 10 ms gap on the access point's path for each slice of an uncompressed
 * image. The erase is the long part, and that still waits outside this call.
 */
static int store_image(void *user, uint32_t offset, const uint8_t *data, uint16_t len)
{
    uint32_t actual = 0;
    int8_t status;

    (void)user;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (flash_bringup() != 0) {
        rtt_puts("store noflash\n");
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (offset == 0u && !image_blank_ok) {
        /* The inflater cannot resend a stage. Pictures are blanked before
         * the first block, so this only happens if that blank was missed.
         */
        if (atc_session.xfer_type == OEPL_TYPE_ZLIB) {
            return -1;
        }
        if (atc_session.xfer_type == OEPL_TYPE_FW) {
            if (!fw_staged) {
                fw_staged = 1;
                staging_erase_begin();
            }
        } else if (!image_erase_on) {
            staging_erase_begin();
        }
        return 2;
    }
    status = spi_flash_write_data((uint8_t *)data, ATC_IMAGE_BASE + offset, len, &actual);
    if (status != SPI_FLASH_ERR_OK || actual != len) {
        rtt_puts("store write fail\n");
        return -1;
    }
    return 0;
}

static int flash_read(uint32_t address, uint8_t *data, uint32_t len)
{
    uint32_t got = 0;

    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (flash_bringup() != 0) {
        return -1;
    }
    if (spi_flash_read_data(data, address, len, &got) != SPI_FLASH_ERR_OK || got != len) {
        return -1;
    }
    return 0;
}

static int flash_write(uint32_t address, const uint8_t *data, uint32_t len)
{
    uint32_t got = 0;

    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (flash_bringup() != 0) {
        return -1;
    }
    if (spi_flash_write_data((uint8_t *)data, address, len, &got) != SPI_FLASH_ERR_OK || got != len) {
        return -1;
    }
    return 0;
}

/* ---- the settings record ------------------------------------------------
 *
 * Read once, before the radio starts, by atc_early_init(). Written only by
 * 0x0004 and 0x0009, and both of those reboot once it is on the flash: the
 * panel geometry is baked into the session at init and the public address is
 * taken by the link layer before any of this code runs, so neither can be
 * changed in place. Storing and rebooting is the honest way to do it, and it
 * is also what the reference firmware does.
 */
static void power_schedule(uint8_t action);

static atc_settings atc_cfg;
static uint8_t cfg_loaded;

static timer_hnd cfg_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t cfg_phase;
static uint8_t cfg_erase_wait;
static uint8_t cfg_drop_image;
static uint8_t cfg_rec[ATC_SETTINGS_BYTES];

static void cfg_load(void)
{
    uint8_t rec[ATC_SETTINGS_BYTES];

    if (cfg_loaded) {
        return;
    }
    cfg_loaded = 1;
    atc_settings_clear(&atc_cfg);
    if (flash_read(ATC_CFG_ADDR, rec, sizeof rec) != 0
        || !atc_settings_decode(rec, &atc_cfg)) {
        /* No record, one from a build that laid it out differently, or one
         * whose check word fails. All three want the same answer: the
         * compiled panel and the address the chip booted with. That is a tag
         * that still works, which beats one that guesses.
         */
        atc_settings_clear(&atc_cfg);
        rtt_puts("cfg default\n");
        return;
    }
    rtt_puts("cfg loaded\n");
}

static void cfg_step(void);

static void cfg_arm(uint32_t delay)
{
    if (cfg_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    cfg_timer = atc_timer(delay, cfg_step);
    if (cfg_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("cfg timer\n");
    }
}

/* One 4 KiB erase, kicked and then polled, never waited on inside a GATT
 * callback. Returns 1 when the sector is gone, 0 to be called again, -1 on a
 * refusal.
 */
static int cfg_erase(uint32_t address)
{
    if (!cfg_erase_wait) {
        int kicked = erase_kick(address);

        if (kicked < 0) {
            return -1;
        }
        if (kicked > 0) {
            return 0;
        }
        cfg_erase_wait = 1;
        return 0;
    }
    {
        int ready = erase_poll();

        if (ready == 0) {
            return 0;
        }
        cfg_erase_wait = 0;
        return (ready < 0) ? -1 : 1;
    }
}

static void cfg_step(void)
{
    cfg_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (cfg_phase == 0u) {
        return;
    }
    if (cfg_phase == 1u) {
        int done = cfg_erase(ATC_CFG_ADDR);

        if (done < 0) {
            rtt_puts("cfg erase fail\n");
            cfg_phase = 0;
            return;
        }
        if (done == 0) {
            cfg_arm(1);
            return;
        }
        cfg_phase = 2;
        cfg_arm(1);
        return;
    }
    if (cfg_phase == 2u) {
        uint8_t got[ATC_SETTINGS_BYTES];

        /* Read back before rebooting on the strength of it. A record that did
         * not land would come up as "no record" and quietly undo the command
         * that was just acknowledged.
         */
        if (flash_write(ATC_CFG_ADDR, cfg_rec, sizeof cfg_rec) != 0
            || flash_read(ATC_CFG_ADDR, got, sizeof got) != 0
            || memcmp(got, cfg_rec, sizeof got) != 0) {
            rtt_puts("cfg write fail\n");
            cfg_phase = 0;
            return;
        }
        rtt_puts("cfg stored\n");
        cfg_phase = 3;
        cfg_arm(1);
        return;
    }
    if (cfg_phase == 3u) {
        /* A new glass is a new picture size, so the picture in flash is the
         * wrong shape and the marker that says one is there has to go. Drop
         * the marker and the next boot draws its own status screen instead of
         * clocking out a picture cut to the old geometry.
         */
        if (cfg_drop_image) {
            int done = cfg_erase(ATC_MARK_ADDR);

            if (done == 0) {
                cfg_arm(1);
                return;
            }
            if (done < 0) {
                rtt_puts("cfg mark fail\n");
            }
            cfg_drop_image = 0;
        }
        cfg_phase = 0;
        rtt_puts("cfg reboot\n");
        power_schedule(1u);
        return;
    }
    cfg_phase = 0;
}

/* Start a store. 0 when one is already running, so a second command cannot
 * interleave with it and leave half of each record on the flash.
 */
static int cfg_store(const atc_settings *next, uint8_t drop_image)
{
    if (cfg_phase != 0u) {
        return 0;
    }
    atc_settings_encode(next, cfg_rec);
    cfg_erase_wait = 0;
    cfg_drop_image = drop_image;
    cfg_phase = 1;
    cfg_arm(1);
    return 1;
}

void atc_early_init(void)
{
    static uint8_t done;
    uint8_t addr[6];

    /* Called from periph_init(), which the SDK runs after it has copied the
     * OTP address into dev_bdaddr and before it starts the stack. That is the
     * only window in which a stored address can still become the one on the
     * air, so the record is read here and nowhere else.
     *
     * Once, though - and the guard matters more than it looks. On the
     * DA14585 periph_init() is not only the boot path: rwble.c calls it again
     * on every wake from extended sleep, whenever the peripheral domain has
     * been off, and it does so inside the BLE wake sequence between powering
     * the radio up and the XTAL16M trim. The SDK says plainly that code added
     * to periph_init() affects the Bluetooth timings there. Reading flash and
     * re-initialising the log buffer in that window would be a poor idea, and
     * none of it needs doing twice: the settings live in retained RAM.
     */
    if (done) {
        return;
    }
    done = 1;
    rtt_init();
    cfg_load();
    if (atc_cfg.config_id != 0u && !atc_panel_select(atc_cfg.config_id)) {
        /* The record names a config this build does not carry - an older
         * firmware put back on a tag somebody had configured. Stay
         * unconfigured rather than refuse to start; a host can set it again.
         */
        rtt_puts("cfg unknown\n");
    }
    if (atc_settings_bd_addr(&atc_cfg, addr)) {
        memcpy(dev_bdaddr.addr, addr, sizeof addr);
        rtt_puts("cfg mac\n");
    }
}

/* 16-byte record at ATC_NDEF_ADDR, then the bare message. Byte 9 is the XOR. */
static uint8_t ndef_crc(const uint8_t hdr[16], const uint8_t *msg, uint16_t len)
{
    uint8_t crc = 0;
    uint16_t i;

    for (i = 0; i < 16u; i++) {
        if (i != 9u) {
            crc ^= hdr[i];
        }
    }
    for (i = 0; i < len; i++) {
        crc ^= msg[i];
    }
    return crc;
}

static void ndef_load(void)
{
    uint8_t hdr[16];
    uint16_t len;

    if (flash_read(ATC_NDEF_ADDR, hdr, sizeof hdr) != 0) {
        return;
    }
    if (hdr[0] != 'N' || hdr[1] != 'D' || hdr[2] != 'E' || hdr[3] != 'F' || hdr[6] != 1u) {
        return;
    }
    len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
    if (len > OEPL_NDEF_MAX) {
        return;
    }
    if (len != 0 && flash_read(ATC_NDEF_ADDR + 16u, atc_session.ndef_msg, len) != 0) {
        return;
    }
    if (ndef_crc(hdr, atc_session.ndef_msg, len) != hdr[9]) {
        return;
    }
    atc_session.ndef_len = len;
    atc_session.ndef_src = hdr[7];
    atc_session.ndef_flags = hdr[8];
    atc_session.ndef_dirty = 0;
    rtt_puts("ndef load\n");
}

static void ndef_commit(void);

static void ndef_arm(uint32_t delay)
{
    if (ndef_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    ndef_timer = atc_timer(delay, ndef_commit);
}

static void ndef_commit(void)
{
    uint8_t hdr[16];
    uint16_t len;

    ndef_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!atc_session.ndef_dirty) {
        ndef_busy = 0;
        ndef_erase_wait = 0;
        return;
    }
    if (refresh_active || refresh_pending || ota_busy || atc_session.active
        || atc_session.store_more) {
        ndef_arm(50);
        return;
    }
    len = atc_session.ndef_len;
    if (len > OEPL_NDEF_MAX) {
        atc_session.ndef_dirty = 0;
        ndef_busy = 0;
        ndef_erase_wait = 0;
        return;
    }
    if (!ndef_erase_wait) {
        int kicked = erase_kick(ATC_NDEF_ADDR);

        if (kicked < 0) {
            ndef_busy = 0;
            rtt_puts("ndef erase\n");
            ndef_arm(100);
            return;
        }
        if (kicked > 0) {
            ndef_arm(1);
            return;
        }
        ndef_erase_wait = 1;
        ndef_busy = 1;
        ndef_arm(1);
        return;
    }
    {
        int ready = erase_poll();

        if (ready == 0) {
            ndef_arm(1);
            return;
        }
        if (ready < 0) {
            ndef_erase_wait = 0;
            ndef_busy = 0;
            rtt_puts("ndef erase\n");
            ndef_arm(100);
            return;
        }
    }
    ndef_erase_wait = 0;
    ndef_busy = 1;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'N';
    hdr[1] = 'D';
    hdr[2] = 'E';
    hdr[3] = 'F';
    hdr[4] = (uint8_t)len;
    hdr[5] = (uint8_t)(len >> 8);
    hdr[6] = 1;
    hdr[7] = atc_session.ndef_src;
    hdr[8] = atc_session.ndef_flags;
    hdr[9] = ndef_crc(hdr, atc_session.ndef_msg, len);
    if (flash_write(ATC_NDEF_ADDR, hdr, sizeof hdr) != 0
        || (len != 0 && flash_write(ATC_NDEF_ADDR + 16u, atc_session.ndef_msg, len) != 0)) {
        ndef_busy = 0;
        rtt_puts("ndef write\n");
        ndef_arm(100);
        return;
    }
    {
        uint8_t got[16];
        uint16_t off;

        if (flash_read(ATC_NDEF_ADDR, got, sizeof got) != 0
            || memcmp(got, hdr, sizeof got) != 0) {
            ndef_busy = 0;
            rtt_puts("ndef verify\n");
            ndef_arm(100);
            return;
        }
        for (off = 0; off < len; ) {
            uint16_t n = (uint16_t)(len - off);

            if (n > sizeof got) {
                n = (uint16_t)sizeof got;
            }
            if (flash_read(ATC_NDEF_ADDR + 16u + off, got, n) != 0
                || memcmp(got, atc_session.ndef_msg + off, n) != 0) {
                ndef_busy = 0;
                rtt_puts("ndef verify\n");
                ndef_arm(100);
                return;
            }
            off = (uint16_t)(off + n);
        }
    }
    atc_session.ndef_dirty = 0;
    ndef_busy = 0;
    rtt_puts("ndef saved\n");
}

/* One slice of the copy, then back to the kernel. A single pass erases and
 * programs the bank for longer than the 6 s supervision timeout, so the
 * success notify never leaves. The header is still written last.
 */
#define OTA_CHUNK 1024u

static oepl_fw_view ota_view;
static uint8_t ota_head[OEPL_FW_HEADER];
static uint8_t ota_chunk[OTA_CHUNK];
static uint32_t ota_file_len;
static uint32_t ota_cursor;
static uint32_t ota_crc;
static uint32_t ota_sectors;
static uint8_t ota_phase;
static uint8_t ota_wiped;
static uint8_t ota_erase_wait;

static void notify_reply(const uint8_t *payload, uint16_t length);
static void power_schedule(uint8_t action);
static void ota_step(void);

static void ota_arm(void)
{
    if (ota_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    ota_timer = atc_timer(1, ota_step);
    if (ota_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("ota timer\n");
    }
}

static void ota_fail(void)
{
    uint8_t reply[2];

    /* The bank header is wiped one sector at a time, then the error goes out. */
    if (ota_wiped && ota_phase != 7u) {
        ota_phase = 7;
        ota_erase_wait = 0;
        ota_busy = 1;
        ota_arm();
        return;
    }
    ota_busy = 0;
    ota_phase = 0;
    ota_wiped = 0;
    ota_erase_wait = 0;
    rtt_puts("ota fail\n");
    oepl_frame_put(reply, OEPL_CMD_ERR);
    notify_reply(reply, 2);
}

static void ota_begin(uint32_t file_len)
{
    ota_file_len = file_len;
    ota_cursor = 0;
    ota_crc = 0;
    ota_sectors = 0;
    ota_wiped = 0;
    ota_erase_wait = 0;
    ota_phase = 1;
    ota_busy = 1;
    memset(&ota_view, 0, sizeof ota_view);
    ota_arm();
}

static void ota_step(void)
{
    uint32_t n;
    uint8_t reply[2];

    /* The handle already fired. A failed re-arm stays invalid and the
     * advertising tick runs the next slice, so a full timer queue cannot
     * leave every later command answered with FFFF.
     */
    ota_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!ota_busy || ota_phase == 0u) {
        return;
    }
    if (ota_phase == 1u) {
        uint32_t head_len = ota_file_len < 80u ? ota_file_len : 80u;

        if (ota_file_len < OEPL_FW_MIN || ota_file_len > OEPL_FW_MAX
            || ATC_IMAGE_BASE + ota_file_len > ATC_FLASH_END
            || flash_read(ATC_IMAGE_BASE, ota_chunk, head_len) != 0
            || !oepl_fw_open(ota_chunk, head_len, ota_file_len, &ota_view)
            || ota_view.image_len > (ATC_BANK_END - ATC_BANK_BASE)
            || flash_read(ATC_IMAGE_BASE + ota_view.image_off + OEPL_FW_HEADER, ota_chunk, 8) != 0
            || !oepl_fw_vectors(ota_chunk)) {
            rtt_puts("ota header\n");
            ota_fail();
            return;
        }
        ota_sectors = (ota_view.image_len + (ATC_SECTOR - 1u)) / ATC_SECTOR;
        if (ATC_BANK_BASE + (ota_sectors * ATC_SECTOR) > ATC_BANK_END) {
            ota_fail();
            return;
        }
        ota_crc = 0;
        ota_cursor = 0;
        ota_phase = 2;
    } else if (ota_phase == 2u) {
        n = ota_view.code_size - ota_cursor;
        if (n > OTA_CHUNK) {
            n = OTA_CHUNK;
        }
        if (flash_read(ATC_IMAGE_BASE + ota_view.image_off + OEPL_FW_HEADER + ota_cursor, ota_chunk, n) != 0) {
            ota_fail();
            return;
        }
        ota_crc = oepl_crc32_update(ota_crc, ota_chunk, n);
        ota_cursor += n;
        if (ota_cursor == ota_view.code_size) {
            if (ota_crc != ota_view.crc) {
                rtt_puts("ota crc\n");
                ota_fail();
                return;
            }
            ota_cursor = 0;
            ota_wiped = 1;
            ota_phase = 3;
            rtt_puts("ota erase\n");
        }
    } else if (ota_phase == 3u) {
        if (!ota_erase_wait) {
            int kicked = erase_kick(ATC_BANK_BASE + (ota_cursor * ATC_SECTOR));

            if (kicked < 0) {
                rtt_puts("ota erase fail\n");
                ota_fail();
                return;
            }
            if (kicked > 0) {
                ota_arm();
                return;
            }
            ota_erase_wait = 1;
            ota_arm();
            return;
        }
        {
            int ready = erase_poll();

            if (ready == 0) {
                ota_arm();
                return;
            }
            if (ready < 0) {
                ota_erase_wait = 0;
                rtt_puts("ota erase fail\n");
                ota_fail();
                return;
            }
        }
        ota_erase_wait = 0;
        ota_cursor++;
        if (ota_cursor == ota_sectors) {
            ota_cursor = OEPL_FW_HEADER;
            ota_phase = 4;
            rtt_puts("ota write\n");
        }
    } else if (ota_phase == 7u) {
        if (!ota_erase_wait) {
            int kicked = erase_kick(ATC_BANK_BASE);

            if (kicked < 0) {
                ota_wiped = 0;
                ota_fail();
                return;
            }
            if (kicked > 0) {
                ota_arm();
                return;
            }
            ota_erase_wait = 1;
            ota_arm();
            return;
        }
        {
            int ready = erase_poll();

            if (ready == 0) {
                ota_arm();
                return;
            }
        }
        ota_erase_wait = 0;
        ota_wiped = 0;
        ota_fail();
        return;
    } else if (ota_phase == 4u) {
        n = ota_view.image_len - ota_cursor;
        if (n > OTA_CHUNK) {
            n = OTA_CHUNK;
        }
        if (flash_read(ATC_IMAGE_BASE + ota_view.image_off + ota_cursor, ota_chunk, n) != 0
            || flash_write(ATC_BANK_BASE + ota_cursor, ota_chunk, n) != 0) {
            ota_fail();
            return;
        }
        ota_cursor += n;
        if (ota_cursor == ota_view.image_len) {
            ota_crc = 0;
            ota_cursor = 0;
            ota_phase = 5;
        }
    } else if (ota_phase == 5u) {
        n = ota_view.code_size - ota_cursor;
        if (n > OTA_CHUNK) {
            n = OTA_CHUNK;
        }
        if (flash_read(ATC_BANK_BASE + OEPL_FW_HEADER + ota_cursor, ota_chunk, n) != 0) {
            ota_fail();
            return;
        }
        ota_crc = oepl_crc32_update(ota_crc, ota_chunk, n);
        ota_cursor += n;
        if (ota_cursor == ota_view.code_size) {
            if (ota_crc != ota_view.crc) {
                rtt_puts("ota verify\n");
                ota_fail();
                return;
            }
            ota_phase = 6;
        }
    } else if (ota_phase == 6u) {
        if (flash_read(ATC_IMAGE_BASE + ota_view.image_off, ota_head, OEPL_FW_HEADER) != 0
            || ota_head[0] != 0x70u || ota_head[1] != 0x51u
            || ota_head[2] != 0xAAu || ota_head[3] != 0x00u
            || flash_write(ATC_BANK_BASE, ota_head, OEPL_FW_HEADER) != 0
            || flash_read(ATC_BANK_BASE, ota_chunk, 4) != 0
            || ota_chunk[0] != 0x70u || ota_chunk[1] != 0x51u || ota_chunk[2] != 0xAAu) {
            ota_fail();
            return;
        }
        rtt_puts("ota stored\n");
        ota_busy = 0;
        ota_phase = 0;
        ota_wiped = 0;
        oepl_frame_put(reply, OEPL_CMD_FW_OK);
        notify_reply(reply, 2);
        rtt_puts("ota reboot\n");
        power_schedule(1);
        return;
    }
    ota_arm();
}

/* Set once a block part arrives, so a late retry does not restart the transfer. */
static uint8_t block_answered;

static void notify_reply(const uint8_t *payload, uint16_t length);
static void deliver_reply(const uint8_t *reply, uint16_t reply_len);

static void write_u64(uint8_t *dst, uint64_t value)
{
    uint8_t i;

    for (i = 0; i < 8u; i++) {
        dst[i] = (uint8_t)(value >> (8u * i));
    }
}

/* ---- the transfer stall guard -------------------------------------------
 *
 * Parts do get lost. A host that keeps more than one write in flight can
 * outrun this chip's receive path, and a write whose kernel message cannot
 * be allocated is dropped with nothing said about it. Measured on this tag:
 * four parts in flight and three vanished inside one block.
 *
 * What used to happen then was nothing at all. The one retry there was fired
 * 300 ms after a block request and gave up as soon as the first part of that
 * block arrived - block_answered - so a part lost in the middle left the tag
 * waiting for ever for something the host believed it had already sent. The
 * upload simply hung, and from the other end that is indistinguishable from
 * the tag having died.
 *
 * So progress is what is watched, not the first part. Every part re-arms this
 * guard. If it fires, the block request goes out again carrying the live
 * mask, which tells the host exactly which parts are still missing, and a
 * lost part costs one guard interval instead of the transfer.
 *
 * The mask itself is the progress signal - if it has not changed since the
 * guard was armed, nothing arrived. That is cheaper than counting parts and
 * it cannot disagree with what the request is about to advertise.
 *
 * Giving up matters as much as retrying. A host that has gone away would
 * otherwise have the tag asking for ever, and an open transfer is not
 * harmless: it makes the tag refuse deep sleep, reboot and the next
 * AvailDataInfo, so one abandoned upload would need a power cycle to clear.
 * After ATC_STALL_TRIES fruitless requests the transfer is abandoned.
 */
#define ATC_STALL_TICKS 40u  /* easy-timer units of 10 ms, so 400 ms */
#define ATC_STALL_TRIES 12u

static timer_hnd stall_timer = EASY_TIMER_INVALID_TIMER;
static uint8_t stall_mask[OEPL_MASK_BYTES];
static uint8_t stall_tries;

static void stall_fire(void);

static void stall_disarm(void)
{
    if (stall_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(stall_timer);
        stall_timer = EASY_TIMER_INVALID_TIMER;
    }
    stall_tries = 0;
}

/* Arm, remembering the mask so the next fire can tell whether anything
 * arrived. Called after a block request goes out and after every part.
 */
static void stall_arm(void)
{
    if (stall_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(stall_timer);
        stall_timer = EASY_TIMER_INVALID_TIMER;
    }
    if (!atc_connected || !atc_session.active) {
        return;
    }
    memcpy(stall_mask, atc_session.mask, OEPL_MASK_BYTES);
    stall_timer = atc_timer(ATC_STALL_TICKS, stall_fire);
}

static void send_block_request(const char *why)
{
    uint8_t rsp[19];
    uint8_t *req;

    oepl_frame_put(rsp, OEPL_CMD_BLOCK_REQ);
    req = rsp + 2;
    memset(req, 0, OEPL_AVAIL_BYTES);
    write_u64(req + 1, atc_session.xfer_ver);
    req[9] = atc_session.block_id;
    req[10] = atc_session.xfer_type;
    memcpy(req + 11, atc_session.mask, OEPL_MASK_BYTES);
    req[0] = oepl_checksum_of(req, OEPL_AVAIL_BYTES);
    rtt_puts(why);
    notify_reply(rsp, 19);
}

static void stall_fire(void)
{
    stall_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!atc_connected || !atc_session.active) {
        stall_tries = 0;
        return;
    }
    /* Work the tag is already doing is not a stall. The host is waiting for
     * us, not the other way round, so leave it alone and look again later.
     */
    if (atc_session.store_more || atc_session.z_more || ota_busy
        || image_erase_on || refresh_active) {
        stall_arm();
        return;
    }
    if (memcmp(stall_mask, atc_session.mask, OEPL_MASK_BYTES) != 0) {
        /* Parts arrived since the last look. Healthy; start the count again. */
        stall_tries = 0;
        stall_arm();
        return;
    }
    stall_tries++;
    if (stall_tries > ATC_STALL_TRIES) {
        rtt_puts("xfer abandoned\n");
        oepl_session_abort(&atc_session);
        stall_disarm();
        {
            uint8_t err[2];

            oepl_frame_put(err, OEPL_CMD_ERR);
            notify_reply(err, 2);
        }
        return;
    }
    send_block_request("stall, asking again\n");
    stall_arm();
}

static void push_packed_again(void)
{
    if (!atc_connected || block_answered || !atc_session.active) {
        return;
    }
    send_block_request("rsp again\n");
    stall_arm();
}

static void image_erase_arm(uint16_t delay)
{
    if (image_erase_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    image_erase_timer = atc_timer(delay, image_erase_tick);
    if (image_erase_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("img timer\n");
    }
}

/* One 4 KiB sector, then back to the kernel. A 64 KB erase inside the write
 * callback holds the link past the 6 s supervision, and the second picture
 * arrives while the first refresh is still reading this same flash.
 */
static void image_erase_tick(void)
{
    image_erase_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!image_erase_on) {
        return;
    }
    if (refresh_active || refresh_pending) {
        refresh_pending = 0;
        refresh_active = 0;
        epd_stop();
        rtt_puts("img stop refresh\n");
    }
    if (flash_bringup() != 0) {
        rtt_puts("img noflash\n");
        image_erase_arm(10);
        return;
    }
    if (!image_sector_wait) {
        int kicked = erase_kick(ATC_IMAGE_BASE + ((uint32_t)image_erase_at * ATC_SECTOR));

        if (kicked < 0) {
            rtt_puts("img erase fail\n");
            image_erase_arm(10);
            return;
        }
        if (kicked > 0) {
            image_erase_arm(1);
            return;
        }
        image_sector_wait = 1;
        image_erase_arm(1);
        return;
    }
    {
        int ready = erase_poll();

        if (ready == 0) {
            image_erase_arm(1);
            return;
        }
        if (ready < 0) {
            image_sector_wait = 0;
            rtt_puts("img erase fail\n");
            image_erase_arm(10);
            return;
        }
    }
    image_sector_wait = 0;
    if ((image_erase_at & 7u) == 0u) {
        rtt_puts("img sector ");
        rtt_u32(image_erase_at);
        rtt_puts("\n");
    }
    image_erase_at++;
    if (image_erase_at < (uint16_t)((ATC_FLASH_END - ATC_IMAGE_BASE) / ATC_SECTOR)) {
        image_erase_arm(1);
        return;
    }
    image_erase_on = 0;
    image_blank_ok = 1;
    image_blank_ver = image_erase_ver;
    rtt_puts("img blank\n");
    if (staging_hold) {
        staging_hold = 0;
        rtt_puts("store blank\n");
        store_arm(1);
        return;
    }
    if (!image_hold_on) {
        return;
    }
    image_hold_on = 0;
    block_answered = 0u;
    notify_reply(image_hold, image_hold_len);
    atc_timer(30, push_packed_again);
}

static int image_plane_type(uint8_t type)
{
    return type == OEPL_TYPE_RAW_1BPP || type == OEPL_TYPE_RAW_2BPP
        || type == OEPL_TYPE_ZLIB;
}

/* 1 when the block request stays here until the picture area is blank.
 * A resume of the same version, after that blank, is sent at once.
 */
static int hold_for_blank(const uint8_t *reply, uint16_t reply_len)
{
    if (reply == 0 || reply_len < 2u || reply_len > sizeof image_hold) {
        return 0;
    }
    if (oepl_frame_get(reply) != OEPL_CMD_BLOCK_REQ) {
        return 0;
    }
    if (!image_plane_type(atc_session.xfer_type) || atc_session.block_id != 0u) {
        return 0;
    }
    if (image_blank_ok && image_blank_ver == atc_session.xfer_ver) {
        return 0;
    }
    memcpy(image_hold, reply, reply_len);
    image_hold_len = reply_len;
    image_hold_on = 1;
    if (!image_erase_on || image_erase_ver != atc_session.xfer_ver) {
        image_erase_ver = atc_session.xfer_ver;
        image_erase_at = 0;
        image_erase_on = 1;
        image_blank_ok = 0;
        image_sector_wait = 0;
        staging_hold = 0;
        refresh_pending = 0;
        refresh_active = 0;
        epd_stop();
        rtt_puts("img erase\n");
        image_erase_arm(1);
    }
    return 1;
}

static void notify_reply(const uint8_t *payload, uint16_t length)
{
    struct custs1_val_ntf_ind_req *req;

    if (!atc_connected || length == 0) {
        return;
    }
    req = KE_MSG_ALLOC_DYN(CUSTS1_VAL_NTF_REQ,
                           prf_get_task_from_id(TASK_ID_CUSTS1),
                           TASK_APP,
                           custs1_val_ntf_ind_req,
                           length);
    req->conidx = atc_conidx;
    req->notification = true;
    req->handle = ATC_IDX_OEPL_VAL;
    req->length = length;
    memcpy(req->value, payload, length);
    KE_MSG_SEND(req);
}

/* ---- the log stream -----------------------------------------------------
 *
 * While a client has asked for it with 0x0022 01, whatever the firmware would
 * have printed on a probe is pushed to it as 0x00D6. A tag with no probe on
 * it can then still be followed, which is most of them.
 *
 * Pushed rather than polled. A read would be a command like any other, the
 * dispatcher logs every command it is given, and that line would then be part
 * of what the next read returned: following a silent tag would cost two
 * commands a second and return the sound of its own asking.
 *
 * Nothing in here logs anything, for the same reason turned inside out.
 * Printing while draining the ring would put the act of draining into the
 * ring and the stream would feed itself for ever.
 */
#define ATC_LOG_CHUNK 200u

static timer_hnd log_timer = EASY_TIMER_INVALID_TIMER;

static void log_step(void);

static void log_arm(uint32_t delay)
{
    if (log_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    log_timer = atc_timer(delay, log_step);
}

/* Called when 0x0022 has just changed the flag, and when the link drops. */
static void log_follow(void)
{
    if (atc_connected && atc_session.log_stream) {
        rtt_log_enable(1);
        log_arm(1);
        return;
    }
    atc_session.log_stream = 0;
    rtt_log_enable(0);
}

static void log_step(void)
{
    uint8_t frame[2u + ATC_LOG_CHUNK];
    uint16_t n;

    log_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!atc_connected || !atc_session.log_stream) {
        atc_session.log_stream = 0;
        rtt_log_enable(0);
        return;
    }
    /* Work in hand comes first - a block request must not queue behind log
     * text. But an open transfer is not work in hand: a picture upload keeps
     * atc_session.active set for a minute or more, and holding the log back
     * for all of it meant the log went quiet exactly when somebody was
     * watching to find out why an upload was going wrong. So only real work
     * defers it, and then only by a tick.
     */
    if (refresh_active || ota_busy || atc_session.store_more
        || atc_session.z_more) {
        log_arm(20);
        return;
    }
    /* A gap is said out loud. Silently closing one would let dropped output
     * pass for continuous output, which is worse than a missing line.
     */
    if (rtt_log_dropped() != 0u) {
        static const char gap[] = "\n<log gap>\n";

        oepl_frame_put(frame, OEPL_CMD_ACK_LOG);
        memcpy(frame + 2, gap, sizeof gap - 1u);
        notify_reply(frame, (uint16_t)(2u + (sizeof gap - 1u)));
    }
    n = rtt_log_take(frame + 2, ATC_LOG_CHUNK);
    if (n != 0u) {
        oepl_frame_put(frame, OEPL_CMD_ACK_LOG);
        notify_reply(frame, (uint16_t)(2u + n));
    }
    /* A full chunk means there is more waiting, so come back promptly. An
     * empty one means the tag is quiet, so come back slowly: an idle stream
     * should cost nothing.
     */
    log_arm((n == ATC_LOG_CHUNK) ? 2u : 10u);
}

static void atc_read_battery(void);

static uint8_t swd_released;
static uint16_t swd_grace = ATC_SWD_GRACE_SECONDS;

/* Hand P1_4 and P1_5 back from the debug module and leave them floating.
 *
 * Floating, not pulled: a pull in either direction is a current path the
 * moment the other end of the line is held at the opposite level, which is
 * the whole problem being fixed here.
 */
static void swd_release(uint16_t seconds)
{
    if (swd_released) {
        return;
    }
    if (swd_grace > seconds) {
        swd_grace = (uint16_t)(swd_grace - seconds);
        return;
    }
    swd_released = 1u;
    SetBits16(SYS_CTRL_REG, DEBUGGER_ENABLE, 0);
    GPIO_ConfigurePin(GPIO_PORT_1, GPIO_PIN_4, INPUT, PID_GPIO, false);
    GPIO_ConfigurePin(GPIO_PORT_1, GPIO_PIN_5, INPUT, PID_GPIO, false);
    atc_pad_mode(1u, 4u, (unsigned)INPUT, (unsigned)PID_GPIO);
    atc_pad_mode(1u, 5u, (unsigned)INPUT, (unsigned)PID_GPIO);
    rtt_puts("swd released\n");
}

static void atc_read_panel_temp(void);

static void refresh_timer(void)
{
    int rc;

    if (refresh_pending) {
        refresh_pending = 0;
        if (refresh_source == 1u && (ota_busy || atc_session.active)) {
            return;
        }
        refresh_active = 1;
        rtt_puts(refresh_source ? "gui start\n" : "refresh start\n");
        if (refresh_source == 1u) {
            epd_begin_draw(gui_paint_row);
        } else {
            epd_begin(ATC_IMAGE_BASE, refresh_type);
        }
    }
    if (!refresh_active) {
        return;
    }
    /* One slice, then back to the kernel. A blocking refresh fills the
     * message heap and the stack halts in platform_reset.
     */
    rc = epd_step();
    if (rc > 0) {
        atc_timer(1, refresh_timer);
        return;
    }
    refresh_active = 0;
    if (rc == 0) {
        rtt_puts("refresh ok\n");
        if (refresh_source == 0u && flash_write(ATC_MARK_ADDR, image_mark, sizeof image_mark) == 0) {
            image_kept = 1u;
            rtt_puts("image kept\n");
        }
    } else {
        rtt_puts("refresh fail\n");
    }
    if (refresh_pending) {
        atc_timer(1, refresh_timer);
    }
}

/* Four 128-byte programs, then back to the kernel. A whole inflated plane
 * inside the write callback is what exhausted the message heap.
 */
#define ZLIB_STEP_FLUSHES 4u

static void zlib_arm(uint32_t delay);

static void zlib_step(void)
{
    uint8_t reply[192];
    uint16_t reply_len;

    zlib_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!atc_session.z_more) {
        return;
    }
    reply_len = oepl_session_zlib_poll(&atc_session, reply, sizeof reply);
    if (atc_session.z_more) {
        zlib_arm(1);
        return;
    }
    rtt_puts("zlib done\n");
    /* The same exit as a part that finished without a pause. This used to
     * repeat deliver_reply()'s body, which meant every change to the way a
     * reply goes out had to be made twice - and the stall guard was the one
     * that found out, because it only ever got wired into one of them.
     */
    deliver_reply(reply, reply_len);
}

static void zlib_arm(uint32_t delay)
{
    if (zlib_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    zlib_timer = atc_timer(delay, zlib_step);
    if (zlib_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("zlib timer\n");
    }
}

static void link_apply(uint8_t mode)
{
    struct gapc_param_update_cmd *cmd;

    link_want = mode;
    if (!atc_connected || link_pending || mode == 0u || link_mode == mode) {
        return;
    }
    cmd = app_easy_gap_param_update_get_active(atc_conidx);
    if (cmd == 0) {
        return;
    }
    if (mode == 2u) {
        cmd->intv_min = ATC_FAST_MIN;
        cmd->intv_max = ATC_FAST_MAX;
    } else {
        cmd->intv_min = ATC_SLOW_MIN;
        cmd->intv_max = ATC_SLOW_MAX;
    }
    cmd->latency = 0;
    cmd->time_out = ATC_LINK_TIMEOUT;
    cmd->ce_len_min = 0;
    cmd->ce_len_max = 0;
    link_inflight = mode;
    link_pending = 1;
    rtt_puts(mode == 2u ? "link fast\n" : "link slow\n");
    app_easy_gap_param_update_start(atc_conidx);
}

static void link_slow_later(void)
{
    if (link_want == 0u || link_want == 1u) {
        link_apply(1);
    }
}

static void power_fire(void)
{
    power_timer = EASY_TIMER_INVALID_TIMER;
    if (refresh_active || refresh_pending || ota_busy || atc_session.active) {
        /* Try again rather than drop it.
         *
         * This used to return and that was the end of it: the action was
         * gone. 0x0004 and 0x0009 made that visible - both store a record
         * and then schedule a reboot to pick it up, and a tag that happened
         * to be busy for those 500 ms kept the old glass or the old address
         * while having acknowledged the change. It looked like the store had
         * failed when it was the reboot that never happened.
         */
        rtt_puts("power busy\n");
        power_timer = atc_timer(50, power_fire);
        return;
    }
    if (power_action == 1u) {
        rtt_puts("reboot\n");
        platform_reset(RESET_AFTER_SUOTA_UPDATE);
    }
    if (power_action == 2u) {
        rtt_puts("deep\n");
        arch_set_deep_sleep(false);
    }
}

static void power_schedule(uint8_t action)
{
    power_action = action;
    if (power_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(power_timer);
    }
    /* 500 ms lets the acknowledgement leave before the radio stops. */
    power_timer = atc_timer(50, power_fire);
}

void user_on_update_params_complete(void)
{
    link_pending = 0;
    link_mode = link_inflight;
    rtt_puts("link ok\n");
    if (atc_connected && link_want != 0u && link_want != link_mode) {
        link_apply(link_want);
    }
}

void user_on_update_params_rejected(const uint8_t status)
{
    link_pending = 0;
    /* Remember the attempt so the next block does not ask again. */
    link_mode = link_inflight;
    rtt_puts("link no ");
    rtt_hex16(status);
    rtt_puts("\n");
}

/* Drop everything an idle tag does not need: the flash into its own
 * power-down, and then the peripheral power domain the flash was holding up.
 *
 * Called from the advertising tick as well as from user_before_sleep(),
 * because with ARCH_SLEEP_OFF the sleep callback never runs and this was the
 * only thing that ever powered the flash down.
 */
static void atc_idle_power_down(void)
{
    if (refresh_active || refresh_pending || ota_busy || ndef_busy
        || image_erase_on || atc_session.active || atc_session.z_more
        || atc_session.store_more || atc_session.ndef_dirty || led_run
        || cfg_phase != 0u) {
        return;
    }
    if (flash_open && spi_flash_power_down() == SPI_FLASH_ERR_OK) {
        flash_open = 0;
    }
    /* The peripheral power domain is deliberately left alone here.
     *
     * Putting PD_PER to sleep once the flash is down looks like the obvious
     * next saving, and it may well be one. But the panel and the lamp are
     * driven straight off the GPIO registers, and whether those keep working
     * with that domain asleep is not something to find out by shipping it:
     * the failure would be a glass that stops refreshing, on a tag that has
     * to be opened up to recover. It wants a measurement with a probe
     * attached, not a guess. atc_read_battery() restores whatever state it
     * found, which is safe either way.
     */
}

void user_before_sleep(void)
{
    atc_idle_power_down();
}

sleep_mode_t user_validate_sleep(sleep_mode_t sleep_mode)
{
    /* mode_active leaves the peripheral domain up, so a refresh slice keeps
     * its pins - and a wake that keeps the domain skips the pad replay in
     * periph_init() entirely.
     *
     * This is the SDK's hook for the job and it was already here; an open
     * transfer and a settings commit were missing from it. A transfer in
     * particular must not sleep deeply between parts: the host is waiting on
     * a notification, not on a wake.
     */
    if (refresh_active || refresh_pending || ota_busy || ndef_busy
        || image_erase_on || atc_session.z_more || atc_session.store_more
        || atc_session.active || atc_session.ndef_dirty || led_run
        || cfg_phase != 0u) {
        return mode_active;
    }
    return sleep_mode;
}

void user_resume_from_sleep(void)
{
    /* The only honest place to count sleeps.
     *
     * app_before_sleep() - user_before_sleep() - is called before sleep
     * *checking* starts, which is every pass of the main loop whether
     * anything sleeps or not. Counting there says nothing about sleeping,
     * and reading it as a sleep count is how this firmware came to be
     * credited with thousands of successful sleeps it had not made.
     */
    if (sleep_woke != 0xFFFFu) {
        sleep_woke++;
    }
    if (!led_run) {
        led_apply();
    }
}

static void deliver_reply(const uint8_t *reply, uint16_t reply_len)
{
    if (atc_session.fw_pending) {
        uint32_t file_len = atc_session.xfer_total;

        atc_session.fw_pending = 0;
        stall_disarm();
        ota_begin(file_len);
        return;
    }
    if (atc_session.show_pending) {
        atc_session.show_pending = 0;
        refresh_type = atc_session.show_type;
        refresh_source = 0;
        refresh_pending = 1;
        atc_timer(10, refresh_timer);
    }
    notify_reply(reply, reply_len);
    /* Keep the stall guard in step with the transfer. A part or a block
     * request means the host is mid-transfer and something more is expected,
     * so watch for it; anything that ends a transfer means nothing more is
     * coming and a guard left running would ask for a block that is done.
     */
    if (reply_len >= 2u) {
        uint16_t out = oepl_frame_get(reply);

        if (out == OEPL_CMD_BLOCK_REQ || out == OEPL_CMD_PART_OK
            || out == OEPL_CMD_PART_BAD) {
            if (out == OEPL_CMD_BLOCK_REQ && atc_session.block_bad != block_bad_said) {
                block_bad_said = atc_session.block_bad;
                rtt_puts("block bad n ");
                rtt_u32(atc_session.block_bad);
                rtt_puts(" size ");
                rtt_u32(atc_session.block_bad_size);
                rtt_puts(" want ");
                rtt_u32(atc_session.chunk);
                rtt_puts(" sum ");
                rtt_u32(atc_session.block_bad_sum);
                rtt_puts("\n");
            }
            stall_arm();
        } else if (out == OEPL_CMD_SHOW || out == OEPL_CMD_ALREADY
                   || out == OEPL_CMD_FW_OK || out == OEPL_CMD_ERR
                   || out == OEPL_CMD_FILE_BIG) {
            stall_disarm();
        }
    }
    if (atc_session.ndef_dirty && !ndef_busy) {
        ndef_arm(2);
    }
}

static void store_step(void)
{
    uint8_t reply[192];
    uint16_t reply_len;

    store_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (!atc_session.store_more) {
        return;
    }
    /* The staging erase arms this when the last sector is ready. */
    if (image_erase_on || !image_blank_ok) {
        return;
    }
    reply_len = oepl_session_store_poll(&atc_session, reply, sizeof reply);
    if (atc_session.store_more) {
        store_arm(1);
        return;
    }
    rtt_puts("store done\n");
    deliver_reply(reply, reply_len);
}

static void store_arm(uint16_t delay)
{
    if (store_timer != EASY_TIMER_INVALID_TIMER) {
        return;
    }
    store_timer = atc_timer(delay, store_step);
    if (store_timer == EASY_TIMER_INVALID_TIMER) {
        rtt_puts("store timer\n");
    }
}

static void on_write(const struct custs1_val_write_ind *ind)
{
    uint8_t reply[192];
    uint16_t reply_len;
    uint16_t cmd;

    /* Enabling notifications writes 0x0001 to the config descriptor.
     * That write is not an OEPL command.
     */
    if (ind->handle != ATC_IDX_OEPL_VAL || ind->length < 2u) {
        /* The access point's subscription write lands here as the config index. */
        rtt_puts("cfg ");
        rtt_hex16(ind->handle);
        rtt_puts(" n ");
        rtt_hex16(ind->length);
        if (ind->length >= 1u) {
            rtt_puts(" ");
            rtt_hex16(ind->value[0]);
        }
        rtt_puts("\n");
        return;
    }
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    cmd = oepl_frame_get(ind->value);
    if (cmd == OEPL_CMD_BLOCK_PART) {
        block_answered = 1u;
    }
    rtt_puts("cmd ");
    rtt_hex16(cmd);
    if (cmd == OEPL_CMD_AVAIL) {
        uint16_t i;
        uint16_t n = ind->length;

        rtt_puts(" n ");
        rtt_hex16(n);
        rtt_puts(" ");
        if (n > 24u) {
            n = 24u;
        }
        for (i = 0; i < n; i++) {
            rtt_hex16(ind->value[i]);
        }
    }
    rtt_puts("\n");
    if (ota_busy) {
        oepl_frame_put(reply, OEPL_CMD_ERR);
        notify_reply(reply, 2);
        return;
    }
    /* The lock, applied to everything.
     *
     * It has to sit here rather than in the protocol module, because the
     * lamp, the reboot, the address and the glass are all answered above that
     * module. A lock that only covered the protocol commands would leave the
     * ones that matter most wide open.
     */
    if (!oepl_pin_allows(&atc_session, cmd)) {
        rtt_puts("locked\n");
        oepl_frame_put(reply, OEPL_CMD_ERR);
        notify_reply(reply, 2);
        return;
    }
    oepl_pin_touch(&atc_session);
    /* Pick the glass. The geometry is read once at init, so this is stored
     * and the tag reboots into it.
     */
    if (cmd == ATC_CMD_SETTYPE) {
        uint16_t want;
        const atc_panel *row;
        atc_settings next;

        if (ind->length < 4u) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        want = (uint16_t)(((uint16_t)ind->value[2] << 8) | ind->value[3]);
        row = atc_panel_find_config(want);
        if (want != ATC_CFG_NONE && (row == 0 || !atc_panel_usable(row))) {
            /* Not a config this build carries, or one whose picture would not
             * fit the window. Refusing beats storing it and coming back up
             * drawing nothing. Zero is not refused: it is the way back to
             * unconfigured.
             */
            rtt_puts("config refused ");
            rtt_hex16(want);
            rtt_puts("\n");
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        if (want == atc_session.screen_type) {
            /* Already this config. Acknowledge without a reboot: a host that
             * sets the type on every connect should not restart the tag.
             */
            oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
            notify_reply(reply, 2);
            return;
        }
        if (refresh_active || refresh_pending || atc_session.active) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        next = atc_cfg;
        next.config_id = want;
        if (!cfg_store(&next, 1u)) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        rtt_puts("config stored ");
        rtt_hex16(want);
        rtt_puts("\n");
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        return;
    }
    /* Set the address. Eight bytes, OEPL order. Stored and rebooted into:
     * the link layer reads its public address before this code runs.
     */
    if (cmd == ATC_CMD_SETMAC) {
        atc_settings next;
        uint8_t probe[6];

        if (ind->length < 2u + ATC_SETTINGS_MAC_BYTES) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        if (refresh_active || refresh_pending || atc_session.active) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        next = atc_cfg;
        next.have_mac = 1;
        memcpy(next.mac, ind->value + 2, ATC_SETTINGS_MAC_BYTES);
        /* All zeros and all ones are not addresses. Storing one would bring
         * the tag up unreachable, and the way back would be a programmer.
         */
        if (!atc_settings_bd_addr(&next, probe)) {
            rtt_puts("mac refused\n");
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        if (!cfg_store(&next, 0u)) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        rtt_puts("mac stored\n");
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        return;
    }
    /* Pick the sleep mode. This build's own command - see OEPL_CMD_SLEEP.
     *
     * ARCH_SLEEP_OFF leaves this chip in active mode for ever, which is where
     * the idle current goes: measured 700 uA with nothing happening. Extended
     * sleep is the fix, and the only reason it is not simply compiled in is
     * that a wake this firmware cannot survive would need a programmer to
     * undo. So it is switched here, kept in RAM, and off after every boot.
     */
    if (cmd == ATC_CMD_SLEEP) {
        uint8_t want = (ind->length >= 3u) ? ind->value[2] : 0u;

        if (refresh_active || refresh_pending || ota_busy || atc_session.active) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        if (want > 1u) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        /* Answer first. If the very next thing the chip does is fail to wake,
         * at least the host knows what it asked for.
         */
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        sleep_mode_on = want;
        rtt_puts(want ? "sleep extended\n" : "sleep off\n");
        arch_set_sleep_mode(want ? ARCH_EXT_SLEEP_ON : ARCH_SLEEP_OFF);
        return;
    }
    if (cmd == ATC_CMD_LED) {
        /* A steady color replaces a running blink. */
        if (led_timer != EASY_TIMER_INVALID_TIMER) {
            app_easy_timer_cancel(led_timer);
            led_timer = EASY_TIMER_INVALID_TIMER;
        }
        led_run = 0;
        led_color = (ind->length >= 3u) ? ind->value[2] : 7u;
        if (led_color > 7u) {
            led_color = 7u;
        }
        led_apply();
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        return;
    }
    if (cmd == ATC_CMD_LEDFLASH) {
        led_pattern_begin(ind->value + 2, (uint16_t)(ind->length - 2u));
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        return;
    }
    if (cmd == ATC_CMD_PERIOD) {
        uint16_t slots = ATC_ADV_SLOTS_DEF;

        /* Applied the next time advertising starts. The live radio is left as it is. */
        if (ind->length >= 4u) {
            slots = (uint16_t)(((uint16_t)ind->value[2] << 8) | ind->value[3]);
            if (slots < ATC_ADV_SLOTS_MIN || slots > ATC_ADV_SLOTS_MAX) {
                slots = ATC_ADV_SLOTS_DEF;
            }
        }
        atc_session.adv_slots = slots;
        rtt_puts("adv slots ");
        rtt_u32(slots);
        rtt_puts("\n");
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        return;
    }
    if (cmd == ATC_CMD_DEEP || cmd == ATC_CMD_REBOOT) {
        if (refresh_active || refresh_pending || ota_busy || atc_session.active) {
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
        oepl_frame_put(reply, OEPL_CMD_ACK_CMD);
        notify_reply(reply, 2);
        power_schedule(cmd == ATC_CMD_REBOOT ? 1u : 2u);
        return;
    }
    if (cmd == OEPL_CMD_DEBUG_STS || cmd == OEPL_CMD_SYSINFO) {
        atc_read_battery();
        atc_read_panel_temp();
    }
    /* A picture needs a glass to be the right size for. While the tag is
     * unconfigured there is no size, and the session would refuse the
     * transfer anyway because nothing matches a zero-by-zero panel - but it
     * would refuse it without saying why. Firmware and NDEF transfers are
     * let through: updating an unconfigured tag has to stay possible.
     */
    if (cmd == OEPL_CMD_AVAIL && !atc_panel_configured() && ind->length >= 2u) {
        const uint8_t *body = ind->value + 2;
        uint16_t body_len = (uint16_t)(ind->length - 2u);
        uint8_t type = 0u;

        if (body_len >= 24u) {
            type = body[20];
        } else if (body_len >= OEPL_AVAIL_BYTES) {
            type = body[13];
        }
        if (type != OEPL_TYPE_FW && type != OEPL_NDEF_RAW && type != OEPL_NDEF_URL) {
            rtt_puts("avail refused, unconfigured\n");
            oepl_frame_put(reply, OEPL_CMD_ERR);
            notify_reply(reply, 2);
            return;
        }
    }
    reply_len = oepl_session_handle(&atc_session, ind->value, ind->length, reply, sizeof reply);
    if (atc_session.z_more) {
        rtt_puts("zlib slice\n");
        zlib_arm(1);
        return;
    }
    if (atc_session.store_more && reply_len == 0u) {
        rtt_puts("store wait\n");
        if (!image_erase_on) {
            store_arm(1);
        }
        return;
    }
    if (cmd == OEPL_CMD_FAST) {
        link_apply(2);
    } else if (cmd == OEPL_CMD_SLOW) {
        link_apply(1);
    }
    /* The session has set or cleared the flag; the ring and the pump are
     * ours. Done after the answer is built so the acknowledgement goes out
     * ahead of the first chunk.
     */
    if (cmd == OEPL_CMD_LOG) {
        log_follow();
    }
    if (cmd == OEPL_CMD_AVAIL) {
        rtt_puts("rsp ");
        rtt_hex16(reply_len);
        rtt_puts(" ");
        if (reply_len >= 2u) {
            rtt_hex16(oepl_frame_get(reply));
        }
        rtt_puts("\n");
        if (reply_len >= 2u && oepl_frame_get(reply) == OEPL_CMD_BLOCK_REQ) {
            /* The picture stays until the first firmware block checks out.
             * The erase then runs one sector per tick, not inside this callback.
             */
            if (atc_session.xfer_type == OEPL_TYPE_FW && atc_session.block_id == 0u) {
                image_blank_ok = 0;
                image_erase_on = 0;
                image_hold_on = 0;
                staging_hold = 0;
                image_sector_wait = 0;
                fw_staged = 0;
            }
            /* The request goes out after the 4 KiB blanks. Arming the retry
             * now would ask for block 0 before the access point may send it.
             */
            if (hold_for_blank(reply, reply_len)) {
                return;
            }
            block_answered = 0u;
            atc_timer(30, push_packed_again);
        }
        if (reply_len >= 2u && oepl_frame_get(reply) == OEPL_CMD_ALREADY) {
            rtt_puts("refresh already\n");
        }
    }
    deliver_reply(reply, reply_len);
}

static void on_info(const struct custs1_att_info_req *req, ke_task_id_t dest, ke_task_id_t src)
{
    struct custs1_att_info_rsp *rsp = KE_MSG_ALLOC(CUSTS1_ATT_INFO_RSP, src, dest, custs1_att_info_rsp);

    rsp->conidx = req->conidx;
    rsp->att_idx = req->att_idx;
    if (req->att_idx == ATC_IDX_OEPL_VAL) {
        rsp->length = ATC_OEPL_MAX_LEN;
        rsp->status = ATT_ERR_NO_ERROR;
    } else {
        rsp->length = 0;
        rsp->status = ATT_ERR_WRITE_NOT_PERMITTED;
    }
    KE_MSG_SEND(rsp);
}

/* Summed VBAT sample. The battery curve treats 1705 as 3.000 V. */
/* Reading the battery costs power, so it is worth doing sparingly.
 *
 * The peripheral power domain has to be up for the ADC, and this used to
 * raise it and leave it raised: PERIPH_SLEEP was cleared on the first read
 * and nothing ever set it again, so the domain stayed powered for the rest of
 * the tag's life. With the read happening every 2 s nobody would have noticed
 * the difference, which is rather the point.
 *
 * Now the domain goes back down afterwards - unless something else still
 * needs it, which on this board means the SPI flash, so the flash's own state
 * decides. The ADC itself is left disabled by adc_get_vbat_sample().
 */
static void atc_read_battery(void)
{
    uint32_t sample;
    uint32_t mv;
    int periph_was_up = (GetWord16(SYS_STAT_REG) & PER_IS_UP) != 0;

    SetBits16(PMU_CTRL_REG, PERIPH_SLEEP, 0);
    {
        uint32_t spins = 100000u;
        while (spins-- != 0u && (GetWord16(SYS_STAT_REG) & PER_IS_UP) == 0u) {
        }
    }
    if ((GetWord16(SYS_STAT_REG) & PER_IS_UP) == 0u) {
        return;
    }
    adc_offset_calibrate(ADC_INPUT_MODE_SINGLE_ENDED);
    sample = adc_get_vbat_sample(false);
    /* Put the domain back however this turns out. A failed read is no reason
     * to leave it powered, and the early returns below are exactly the paths
     * that used to leak it.
     */
    if (!periph_was_up && !flash_open) {
        SetBits16(PMU_CTRL_REG, PERIPH_SLEEP, 1);
    }
    if (sample == 0) {
        return;
    }
    mv = sample * 3000u / 1705u;
    if (mv == 0 || mv > 4500u) {
        return;
    }
    atc_session.battery_raw = (uint16_t)sample;
    atc_session.battery_mv = (uint16_t)mv;
}

/* The controller sensor is powered only for this read. A few misses are enough;
 * after that a status poll must not cycle the panel again.
 */
static void atc_read_panel_temp(void)
{
    int8_t degrees;

    if (refresh_active || refresh_pending || temp_valid || temp_tries >= 4u) {
        return;
    }
    temp_tries++;
    if (epd_read_celsius(&degrees) == 0) {
        atc_session.temperature = degrees;
        temp_valid = 1;
        rtt_puts("temp ");
        rtt_u32((uint32_t)(int32_t)degrees);
        rtt_puts("\n");
    } else {
        rtt_puts("temp none\n");
    }
    led_apply();
}

static char gui_name[16];
static char gui_build[28];
static char gui_tag[28];
static char gui_hw[40];
static char gui_bat[40];
static gui_line gui_lines[8];

static char *gui_put(char *p, char *end, const char *text)
{
    while (*text != 0 && p < end) {
        *p++ = *text++;
    }
    return p;
}

static char *gui_put_u(char *p, char *end, uint32_t value)
{
    char tmp[10];
    uint8_t n = 0u;

    if (value == 0u) {
        tmp[n++] = '0';
    }
    while (value != 0u && n < sizeof tmp) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n != 0u && p < end) {
        *p++ = tmp[--n];
    }
    return p;
}

static char *gui_put_hex2(char *p, char *end, uint8_t value)
{
    static const char hexd[] = "0123456789ABCDEF";

    if (p < end) {
        *p++ = hexd[value >> 4];
    }
    if (p < end) {
        *p++ = hexd[value & 0x0Fu];
    }
    return p;
}

static void gui_finish(char *p, char *end)
{
    if (p < end) {
        *p = 0;
    } else {
        end[-1] = 0;
    }
}

static void gui_fill(void)
{
    char *p;
    char *end;
    uint8_t i;
    int8_t degrees = atc_session.temperature;

    p = gui_put(gui_build, gui_build + sizeof gui_build, __DATE__);
    p = gui_put(p, gui_build + sizeof gui_build, " ");
    p = gui_put(p, gui_build + sizeof gui_build, __TIME__);
    gui_finish(p, gui_build + sizeof gui_build);

    /* The name a scanner shows. It is built from the address in
     * atc_mac_name(), so taking it from device_info rather than rebuilding it
     * here means the glass cannot disagree with the air.
     */
    p = gui_put(gui_name, gui_name + sizeof gui_name,
                (const char *)device_info.dev_name.name);
    gui_finish(p, gui_name + sizeof gui_name);

    p = gui_put(gui_tag, gui_tag + sizeof gui_tag, "Tag: ");
    for (i = 0u; i < 6u; i++) {
        if (i != 0u) {
            p = gui_put(p, gui_tag + sizeof gui_tag, ":");
        }
        p = gui_put_hex2(p, gui_tag + sizeof gui_tag, atc_session.mac[i]);
    }
    gui_finish(p, gui_tag + sizeof gui_tag);

    p = gui_put(gui_hw, gui_hw + sizeof gui_hw, "ID ");
    p = gui_put_hex2(p, gui_hw + sizeof gui_hw, (uint8_t)(atc_session.hw_type >> 8));
    p = gui_put_hex2(p, gui_hw + sizeof gui_hw, (uint8_t)atc_session.hw_type);
    p = gui_put(p, gui_hw + sizeof gui_hw, " ");
    p = gui_put_u(p, gui_hw + sizeof gui_hw, atc_session.width);
    p = gui_put(p, gui_hw + sizeof gui_hw, "x");
    p = gui_put_u(p, gui_hw + sizeof gui_hw, atc_session.height);
    gui_finish(p, gui_hw + sizeof gui_hw);

    p = gui_put(gui_bat, gui_bat + sizeof gui_bat, "Good V:");
    p = gui_put_u(p, gui_bat + sizeof gui_bat, atc_session.fw_version);
    p = gui_put(p, gui_bat + sizeof gui_bat, " Bat:");
    p = gui_put_u(p, gui_bat + sizeof gui_bat, atc_session.battery_mv);
    p = gui_put(p, gui_bat + sizeof gui_bat, "mV ");
    if (degrees < 0) {
        p = gui_put(p, gui_bat + sizeof gui_bat, "-");
        degrees = (int8_t)(-degrees);
    }
    p = gui_put_u(p, gui_bat + sizeof gui_bat, (uint32_t)degrees);
    p = gui_put(p, gui_bat + sizeof gui_bat, "'C");
    gui_finish(p, gui_bat + sizeof gui_bat);

    gui_lines[0].text = "BLE OpenEpaperLink";
    gui_lines[0].weight = 2;
    /* The BLE name, big, right under the title: it is what you type into a
     * scanner's filter, so it is the one thing on here somebody reads off the
     * glass before they can talk to the tag at all.
     */
    gui_lines[1].text = gui_name;
    gui_lines[1].weight = 2;
    gui_lines[2].text = "ATC1441 Compiled on:";
    gui_lines[2].weight = 1;
    gui_lines[3].text = gui_build;
    gui_lines[3].weight = 1;
    gui_lines[4].text = gui_tag;
    gui_lines[4].weight = 1;
    gui_lines[5].text = gui_hw;
    gui_lines[5].weight = 1;
    gui_lines[6].text = "Boot";
    gui_lines[6].weight = 2;
    gui_lines[7].text = gui_bat;
    gui_lines[7].weight = 1;
}

static void gui_arm(void)
{
    /* Drawn on every boot, stored picture or not.
     *
     * It used to skip this whenever a picture was already on the glass, the
     * idea being not to throw that picture away. In practice it meant a tag
     * that had ever been given a picture never showed its status screen
     * again - so after a reboot there was no way to read its BLE name, its
     * firmware version or its battery off the glass, which is the one thing
     * the screen is for when something is wrong. The picture is still in
     * flash and the next AvailDataInfo redraws it.
     */
    if (boot_done || refresh_active || refresh_pending || ota_busy || atc_session.active) {
        return;
    }
    if (!atc_panel_configured()) {
        /* No glass named, so there is no geometry to draw into. Writing a
         * screen at a guessed size into an unknown panel is worse than
         * leaving the glass alone, so the tag advertises and waits.
         */
        boot_done = 1u;
        rtt_puts("gui skipped, unconfigured\n");
        return;
    }
    boot_done = 1u;
    gui_fill();
    gui_set_lines(gui_lines, 8);
    refresh_source = 1u;
    refresh_pending = 1u;
    rtt_puts("gui arm\n");
    atc_timer(10, refresh_timer);
}

static void atc_sense_once(void)
{
    atc_read_battery();
    atc_read_panel_temp();
    rtt_puts("sense ");
    rtt_u32(atc_session.battery_mv);
    rtt_puts(" ");
    rtt_u32((uint32_t)(int32_t)atc_session.temperature);
    rtt_puts("\n");
    /* The first power-up of the panel sensor often shifts zeros. A later
     * pass is the one that returns degrees. Stop once it does.
     */
    if (!temp_valid && temp_tries < 4u && atc_connected == 0u) {
        atc_timer(200, atc_sense_once);
        return;
    }
    gui_arm();
}

/* addr[0] is the last octet a scanner prints. The name uses the last three. */
static void atc_mac_name(uint8_t name[10])
{
    static const char hexd[] = "0123456789ABCDEF";
    uint8_t i;

    name[0] = 'A';
    name[1] = 'T';
    name[2] = 'C';
    name[3] = '_';
    for (i = 0; i < 3u; i++) {
        uint8_t b = dev_bdaddr.addr[2u - i];
        name[4u + (2u * i)] = (uint8_t)hexd[b >> 4];
        name[5u + (2u * i)] = (uint8_t)hexd[b & 0x0Fu];
    }
    for (i = 0; i < 6u; i++) {
        atc_session.mac[i] = dev_bdaddr.addr[5u - i];
    }
    /* The eight-byte OEPL address: the company id in front of the six on the
     * air. Sysinfo reports both, and a stored address keeps whatever prefix it
     * was given rather than having 0x1337 forced back on to it.
     */
    if (atc_cfg.have_mac) {
        memcpy(atc_session.mac8, atc_cfg.mac, sizeof atc_session.mac8);
    } else {
        atc_session.mac8[0] = 0x13u;
        atc_session.mac8[1] = 0x37u;
        memcpy(atc_session.mac8 + 2, atc_session.mac, 6);
    }
    device_info.dev_name.length = 10;
    memcpy(device_info.dev_name.name, name, 10);
    device_info.dev_name.name[10] = 0;
}

static void atc_load_adv(uint8_t *adv, uint8_t *adv_len, uint8_t *scan, uint8_t *scan_len)
{
    uint8_t name[10];
    static const uint8_t scan_rsp[] = {0x03, 0x03, 0x37, 0x13};

    atc_mac_name(name);
    *adv_len = oepl_adv_payload(adv, (uint8_t)(ADV_DATA_LEN - 3),
                                atc_session.hw_type, atc_session.fw_version,
                                atc_session.capabilities, atc_session.battery_mv,
                                atc_session.temperature, atc_counter, name, 10);
    atc_counter++;
    memcpy(scan, scan_rsp, sizeof scan_rsp);
    *scan_len = (uint8_t)sizeof scan_rsp;
}

void user_advertise(void);

/* Two seconds while there is work to pump, ten when there is not.
 *
 * Re-arming at 2 s unconditionally gave an idle tag a second wake on top of
 * the radio's own: two wakes per two seconds, 110 to 170 ms apart because the
 * two periods drift past each other, each paying for a crystal start and a
 * pass through the stack. Everything this tick pumps - OTA, the NDEF write, a
 * settings commit, the lamp, a transfer - only exists while something is
 * going on. When nothing is, the tick has a PIN timeout and a once-a-minute
 * battery pass to keep, and ten seconds is plenty for both.
 */
static uint32_t atc_tick_delay(void)
{
    if (ota_busy || ndef_busy || atc_session.ndef_dirty || refresh_active
        || refresh_pending || image_erase_on || atc_session.store_more
        || atc_session.z_more || atc_session.active || led_run
        || atc_session.log_stream || cfg_phase != 0u || atc_connected) {
        return ATC_TICK_BUSY;
    }
    return ATC_TICK_IDLE;
}

static void atc_adv_tick(void)
{
    uint8_t adv[ADV_DATA_LEN];
    uint8_t scan[SCAN_RSP_DATA_LEN];
    uint8_t adv_len = 0;
    uint8_t scan_len = 0;
    uint32_t tick_seconds = atc_tick_delay() / 100u;

    atc_adv_timer = EASY_TIMER_INVALID_TIMER;
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (ota_busy && ota_timer == EASY_TIMER_INVALID_TIMER) {
        ota_step();
    }
    if (atc_session.ndef_dirty && ndef_timer == EASY_TIMER_INVALID_TIMER) {
        ndef_arm(1);
    }
    if (image_erase_on && image_erase_timer == EASY_TIMER_INVALID_TIMER) {
        image_erase_tick();
    }
    if (atc_session.store_more && !image_erase_on
        && store_timer == EASY_TIMER_INVALID_TIMER) {
        store_step();
    }
    if (atc_session.z_more && zlib_timer == EASY_TIMER_INVALID_TIMER) {
        zlib_step();
    }
    if (led_run && led_timer == EASY_TIMER_INVALID_TIMER) {
        led_advance();
    }
    /* This tick is the tag's only clock. Both unlock timeouts are counted in
     * it, so they are accurate to one tick - ample for a 60 s idle window and
     * a 600 s ceiling.
     */
    if (sleep_woke != sleep_woke_said) {
        sleep_woke_said = sleep_woke;
        rtt_puts("woke ");
        rtt_u32(sleep_woke);
        rtt_puts("\n");
    }
#if ATC_POWER_PROBE
    if (probe_quiet) {
        return;
    }
#endif
    oepl_pin_tick(&atc_session, (uint16_t)tick_seconds);
    swd_release((uint16_t)tick_seconds);
    if (atc_session.log_stream && log_timer == EASY_TIMER_INVALID_TIMER) {
        log_step();
    }
    /* A stopped radio is not brought back by an advertising-data update. */
    if (!atc_connected && !adv_on) {
        user_advertise();
        return;
    }
    if (!atc_connected && adv_on) {
        /* The battery is sampled once a minute, not on every tick.
         *
         * It used to be every tick, so every 2 s this tag ran an ADC offset
         * calibration, raised the peripheral power domain and took two
         * samples - all of it to refresh a millivolt field in an
         * advertisement that nothing acts on. A battery does not move in two
         * seconds. Once a minute is as much as the number deserves, and the
         * advertisement in between simply carries the last one.
         */
        if (sense_countdown <= tick_seconds) {
            sense_countdown = ATC_SENSE_SECONDS;
            atc_read_battery();
            /* Only now is there anything new to say. Rewriting the
             * advertisement on every tick meant a GAPM round trip, and the
             * payload it carried was the same numbers again.
             */
            atc_load_adv(adv, &adv_len, scan, &scan_len);
            if (adv_len != 0) {
                app_easy_gap_update_adv_data(adv, adv_len, scan, scan_len);
            }
        } else {
            sense_countdown = (uint16_t)(sense_countdown - tick_seconds);
        }
        /* Nothing wants the flash while the tag is just advertising, and it
         * holds its own power-down. This used to live only in
         * user_before_sleep(), which with ARCH_SLEEP_OFF is never called at
         * all - so the flash stayed awake from the first read until the tag
         * was unplugged.
         */
        atc_idle_power_down();
    }
    atc_adv_timer = atc_timer(atc_tick_delay(), atc_adv_tick);
}

#if ATC_POWER_PROBE
/* A staircase in the current trace, so the sleep floor can be attributed
 * instead of guessed at.
 *
 * Every ATC_PROBE_SECS the tag switches one more thing off and goes back to
 * sleep. One capture then shows a step per phase, and the step where the
 * floor drops names the consumer. The phases are cumulative on purpose: a
 * saving that only appears once two things are both quiet would otherwise be
 * invisible.
 *
 * Each phase announces itself first, by spinning the CPU for its own number
 * times 100 ms. That plateau is around 1 mA against a floor of a tenth of
 * that, so the phase number can be counted off the trace. Working the phase
 * boundaries out from the clock instead got them wrong once, and the last
 * phase puts the chip in deep sleep - after which its RAM, and any log in it,
 * is gone.
 *
 * Diagnostic scaffolding. Compiled out unless ATC_POWER_PROBE is set, and a
 * tag built with it stops advertising and ends up needing a power cycle.
 */
#define ATC_PROBE_SECS 12u

typedef struct {
    uint8_t port;
    uint8_t pin;
} probe_pad;

static const probe_pad probe_epd_pads[] = {
    {EPD_SCLK_PORT, EPD_SCLK_PIN}, {EPD_MOSI_PORT, EPD_MOSI_PIN},
    {EPD_CS_PORT, EPD_CS_PIN},     {EPD_DC_PORT, EPD_DC_PIN},
    {EPD_RST_PORT, EPD_RST_PIN},   {EPD_BS_PORT, EPD_BS_PIN},
    {EPD_PWR_PORT, EPD_PWR_PIN},   {EPD_BUSY_PORT, EPD_BUSY_PIN},
};
/* The lamp pads are never driven low here. They are active low - driving them
 * low lights the lamp, which showed up as 21.5 mA for a whole phase and said
 * nothing about the floor. Floating them is the only useful test.
 */
static const probe_pad probe_lamp_pads[] = {
    {LED_R_PORT, LED_R_PIN}, {LED_G_PORT, LED_G_PIN}, {LED_B_PORT, LED_B_PIN},
    {LED_X_PORT, LED_X_PIN}, {LED_Y_PORT, LED_Y_PIN}, {LED_A_PORT, LED_A_PIN},
    {LED_C_PORT, LED_C_PIN}, {LED_D_PORT, LED_D_PIN}, {LED_E_PORT, LED_E_PIN},
    {LED_F_PORT, LED_F_PIN}, {LED_Z_PORT, LED_Z_PIN},
};
static const probe_pad probe_flash_pads[] = {
    {FLASH_CLK_PORT, FLASH_CLK_PIN}, {FLASH_MOSI_PORT, FLASH_MOSI_PIN},
    {FLASH_MISO_PORT, FLASH_MISO_PIN}, {FLASH_CS_PORT, FLASH_CS_PIN},
};

#define PROBE_N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static uint8_t probe_phase;

static void probe_mark(uint8_t n)
{
    uint8_t i;
    uint16_t j;

    for (i = 0; i < n; i++) {
        for (j = 0; j < 100u; j++) {
            arch_asm_delay_us(1000);
        }
    }
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
}

static void probe_drive_low(const probe_pad *list, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        GPIO_ConfigurePin((GPIO_PORT)list[i].port, (GPIO_PIN)list[i].pin,
                          OUTPUT, PID_GPIO, false);
        atc_pad_mode(list[i].port, list[i].pin, (unsigned)OUTPUT,
                     (unsigned)PID_GPIO);
        atc_pad_level(list[i].port, list[i].pin, 0);
    }
}

static void probe_float(const probe_pad *list, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        GPIO_ConfigurePin((GPIO_PORT)list[i].port, (GPIO_PIN)list[i].pin,
                          INPUT, PID_GPIO, false);
        atc_pad_mode(list[i].port, list[i].pin, (unsigned)INPUT,
                     (unsigned)PID_GPIO);
    }
}

static void probe_tick(void)
{
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    probe_phase++;
    rtt_puts("probe phase ");
    rtt_u32(probe_phase);
    rtt_puts("\n");
    probe_mark(probe_phase);
    switch (probe_phase) {
    case 1u:
        /* The radio first, so the bursts leave the trace and every later step
         * is read against a flat line.
         */
        probe_quiet = 1u;
        if (atc_adv_timer != EASY_TIMER_INVALID_TIMER) {
            app_easy_timer_cancel(atc_adv_timer);
            atc_adv_timer = EASY_TIMER_INVALID_TIMER;
        }
        app_easy_gap_advertise_stop();
        break;
    case 2u:
        /* Put the panel's chip-select and reset back to resting high, the way
         * they were before panel_quiet() was changed. If driving a pad into
         * an unpowered controller costs current, this is where it shows.
         */
        atc_pad_level(EPD_CS_PORT, EPD_CS_PIN, 1);
        atc_pad_level(EPD_RST_PORT, EPD_RST_PIN, 1);
        GPIO_ConfigurePin(EPD_CS_PORT, EPD_CS_PIN, OUTPUT, PID_GPIO, true);
        GPIO_ConfigurePin(EPD_RST_PORT, EPD_RST_PIN, OUTPUT, PID_GPIO, true);
        break;
    case 3u:
        /* And back to low, which should undo exactly phase 2. */
        atc_pad_level(EPD_CS_PORT, EPD_CS_PIN, 0);
        atc_pad_level(EPD_RST_PORT, EPD_RST_PIN, 0);
        GPIO_ConfigurePin(EPD_CS_PORT, EPD_CS_PIN, OUTPUT, PID_GPIO, false);
        GPIO_ConfigurePin(EPD_RST_PORT, EPD_RST_PIN, OUTPUT, PID_GPIO, false);
        break;
    case 4u:
        /* Hand P1_4 and P1_5 back from the debug module and float them. */
        SetBits16(SYS_CTRL_REG, DEBUGGER_ENABLE, 0);
        GPIO_ConfigurePin(GPIO_PORT_1, GPIO_PIN_4, INPUT, PID_GPIO, false);
        GPIO_ConfigurePin(GPIO_PORT_1, GPIO_PIN_5, INPUT, PID_GPIO, false);
        atc_pad_mode(1u, 4u, (unsigned)INPUT, (unsigned)PID_GPIO);
        atc_pad_mode(1u, 5u, (unsigned)INPUT, (unsigned)PID_GPIO);
        break;
    case 5u:
        /* The whole SoC off. What the meter still reads is the rest of the
         * board, and no firmware change gets below it.
         */
        rtt_puts("probe deep sleep\n");
        arch_set_deep_sleep(false);
        break;
    default:
        break;
    }
    if (probe_phase < 5u) {
        atc_timer(ATC_PROBE_SECS * 100u, probe_tick);
    }
}

static void probe_arm(void)
{
    static uint8_t armed;

    if (armed == 0u) {
        armed = 1u;
        atc_timer(ATC_PROBE_SECS * 100u, probe_tick);
    }
}
#endif /* ATC_POWER_PROBE */

void user_advertise(void)
{
    struct gapm_start_advertise_cmd *cmd;
    uint8_t adv_len = 0;
    uint8_t scan_len = 0;

    wdg_reload(WATCHDOG_DEFAULT_PERIOD);
    if (atc_adv_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(atc_adv_timer);
        atc_adv_timer = EASY_TIMER_INVALID_TIMER;
    }
    atc_read_battery();
    cmd = app_easy_gap_undirected_advertise_get_active();
    {
        uint16_t slots = ATC_ADV_SLOTS_DEF;

        /* user_adv_conf is only copied when this message is first created. */
        if (atc_session.adv_slots >= ATC_ADV_SLOTS_MIN
            && atc_session.adv_slots <= ATC_ADV_SLOTS_MAX) {
            slots = (uint16_t)atc_session.adv_slots;
        }
        cmd->intv_min = slots;
        cmd->intv_max = slots;
    }
    atc_load_adv(cmd->info.host.adv_data, &adv_len,
                 cmd->info.host.scan_rsp_data, &scan_len);
    if (adv_len != 0) {
        cmd->info.host.adv_data_len = adv_len;
        cmd->info.host.scan_rsp_data_len = scan_len;
    }
    rtt_puts("adv ");
    rtt_puts((const char *)device_info.dev_name.name);
    rtt_puts("\n");
    app_easy_gap_undirected_advertise_start();
    adv_on = 1;
    atc_adv_timer = atc_timer(200, atc_adv_tick);
    /* The easy-timer queue is not running yet in user_app_on_init. Arm the
     * sensor pass from here, once advertising is actually up.
     */
    if (sense_armed == 0u) {
        sense_armed = 1u;
        atc_timer(100, atc_sense_once);
    }
#if ATC_POWER_PROBE
    probe_arm();
#endif
}

void user_on_dev_config(void)
{
    rtt_puts("dev config\n");
    default_app_on_set_dev_config_complete();
}

void user_on_db_init(void)
{
    rtt_puts("db ready\n");
    default_app_on_db_init_complete();
}

void user_on_adv_complete(const uint8_t status)
{
    adv_on = 0;
#if ATC_POWER_PROBE
    if (probe_quiet) {
        rtt_puts("probe radio off" "\n");
        return;
    }
#endif
    rtt_puts("adv ");
    rtt_hex16(status);
    rtt_puts("\n");
    if (atc_connected != 0) {
        return;
    }
    /* One kernel turn later. A connection indication already queued is applied
     * before the radio is started again. Status 0 with nobody connected used
     * to leave the radio stopped for good.
     */
    if (atc_adv_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(atc_adv_timer);
        atc_adv_timer = EASY_TIMER_INVALID_TIMER;
    }
    atc_adv_timer = atc_timer(1, atc_adv_tick);
}

void user_app_on_init(void)
{
    /* rtt_init() and the settings record are already done, in periph_init()
     * by way of atc_early_init(). Calling rtt_init() again here would throw
     * away everything the early path logged.
     */
    rtt_puts("boot ATC_OEPL\n");
    {
        const atc_panel *panel = atc_panel_get();
        oepl_panel shape;

        shape.width = panel->width;
        shape.height = panel->height;
        shape.colors = panel->colors;
        oepl_session_init(&atc_session, &shape, store_image, 0);
        atc_session.z_step_max = ZLIB_STEP_FLUSHES;
        atc_session.hw_type = panel->hw_type;
        atc_session.screen_type = panel->config_id;
        rtt_puts("config ");
        rtt_hex16(panel->config_id);
        rtt_puts(" ");
        rtt_puts(panel->name);
        rtt_puts(atc_panel_has(ATC_CAP_LED) ? " led" : " no-led");
        rtt_puts(atc_panel_has(ATC_CAP_NFC) ? " nfc" : " no-nfc");
        rtt_puts("\n");
        if (!atc_panel_configured()) {
            /* Nothing is drawn and no picture is taken until a host says
             * which board this is. The glass keeps whatever is on it.
             */
            rtt_puts("unconfigured, waiting for 0004\n");
        }
    }
    atc_session.capabilities = ATC_CAPABILITIES;
    atc_session.fw_version = ATC_FW_VERSION;
    led_apply();
    default_app_on_init();
    flash_bringup();
    {
        uint8_t mark[8];

        if (flash_read(ATC_MARK_ADDR, mark, sizeof mark) == 0
            && memcmp(mark, image_mark, sizeof mark) == 0) {
            image_kept = 1u;
            rtt_puts("image kept\n");
        }
    }
    ndef_load();
}

void user_on_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param)
{
    atc_conidx = connection_idx;
    atc_connected = 1;
    adv_on = 0;
    link_mode = 0;
    link_want = 0;
    link_inflight = 0;
    link_pending = 0;
    /* The tick stays armed. It restarts advertising if this link ends without
     * the disconnect callback, and it reloads the watchdog while connected.
     */
    rtt_puts("link up ");
    rtt_u32(param->con_interval);
    rtt_puts("\n");
    default_app_on_connection(connection_idx, param);
    /* Notifications stay armed for this link. A client that subscribes still
     * writes the same bit. One that misses the descriptor write would otherwise
     * never see the block request.
     */
    custs1_set_ccc_value(connection_idx, ATC_IDX_OEPL_NTF_CFG, PRF_CLI_START_NTF);
    /* Leave the access point's connection interval alone. Asking for a new one
     * here makes that controller drop the block-request notification.
     */
}

void user_on_disconnect(struct gapc_disconnect_ind const *param)
{
    atc_connected = 0;
    link_mode = 0;
    link_want = 0;
    link_inflight = 0;
    link_pending = 0;
    /* Streaming stops with the link, so a tag is never left pushing its log
     * at whoever connects next.
     */
    log_follow();
    /* Nobody left to ask. A guard still running would re-send block requests
     * into a closed link until it gave up on its own.
     */
    stall_disarm();
    /* And it locks again, if a PIN is set. An unlock belongs to the client
     * that gave the PIN, not to the next one along.
     */
    oepl_pin_lock(&atc_session);
    rtt_puts("link down ");
    rtt_hex16(param->reason);
    rtt_puts("\n");
    default_app_on_disconnect(param);
}

void user_catch_rest_hndl(ke_msg_id_t const msgid,
                          void const *param,
                          ke_task_id_t const dest_id,
                          ke_task_id_t const src_id)
{
    switch (msgid) {
    case CUSTS1_VAL_WRITE_IND:
        on_write((const struct custs1_val_write_ind *)param);
        break;
    case CUSTS1_ATT_INFO_REQ:
        on_info((const struct custs1_att_info_req *)param, dest_id, src_id);
        break;
    case GAPC_PARAM_UPDATED_IND: {
        struct gapc_param_updated_ind const *upd = (struct gapc_param_updated_ind const *)param;

        rtt_puts("link iv ");
        rtt_u32(upd->con_interval);
        rtt_puts(" to ");
        rtt_u32(upd->sup_to);
        rtt_puts("\n");
        break;
    }
    case GATTC_EVENT_REQ_IND: {
        struct gattc_event_ind const *ind = (struct gattc_event_ind const *)param;
        struct gattc_event_cfm *cfm = KE_MSG_ALLOC(GATTC_EVENT_CFM, src_id, dest_id, gattc_event_cfm);
        cfm->handle = ind->handle;
        KE_MSG_SEND(cfm);
        break;
    }
    default:
        break;
    }
}

extern int main(void);

void _start(void)
{
    rtt_init();
    rtt_puts("boot enter\n");
    main();
    for (;;) {
    }
}
