#include "user_periph_setup.h"
#include "datasheet.h"
#include "panel.h"
#include "user_atc.h"

/* ---- the pad configuration, and why it is a table ------------------------
 *
 * periph_init() is not only the boot path. On the DA14585 the stack calls it
 * again on every wake whose sleep was long enough for the peripheral power
 * domain to go down - rwble.c, `if (GetBits16(SYS_STAT_REG, PER_IS_DOWN))` -
 * and it does so inside the BLE wake sequence, between powering the radio up
 * and the XTAL16M trim. The SDK's own note in platform/arch/arch.h puts the
 * budget there at roughly 400 us on RCX, and says that overrunning it makes
 * the wake miss its event; with CFG_DEVELOPMENT_DEBUG off there is no assert
 * to say so.
 *
 * That is why short sleeps worked here and the first long one did not. A
 * 10 ms sleep between panel refresh steps leaves the peripheral domain up, so
 * periph_init() is never called and the wake is nearly free. A two-second
 * sleep drops the domain, and then the wake pays for two dozen
 * GPIO_ConfigurePin() calls - each one a ROM call doing a bounds check, a
 * data-register write and a mode-register write.
 *
 * So the pad state is reduced to what it actually is: a flat list of register
 * writes. The list is built once, on the cold boot, by the same code that
 * configures the pads in the first place, so there is no second copy of the
 * pin list to keep in step. The wake path replays it - no calls, no branches,
 * no per-pin arithmetic.
 */
#define PAD_MAX 32u

struct pad_write {
    uint16_t off;   /* offset from GPIO_BASE */
    uint16_t val;   /* mode | function */
};

static struct pad_write pad_mode[PAD_MAX];
static uint8_t pad_mode_n;
/* Output levels, one mask per port. Index 4 is P3 - see pad_port_index(). */
static uint16_t pad_hi[5];
static uint16_t pad_lo[5];
static uint8_t pads_built;

/* P3's registers sit where a fifth port's would: P3_DATA_REG is
 * GPIO_BASE + (4 << 5), not (3 << 5). The SDK calls this the remap and
 * applies it to the mode registers and the data registers alike.
 */
static unsigned pad_port_index(unsigned port)
{
    return (port == 3u) ? 4u : port;
}

static void pad_record(unsigned port, unsigned pin, unsigned mode, unsigned func,
                       bool high)
{
    unsigned p = pad_port_index(port);
    uint16_t bit = (uint16_t)(1u << pin);

    if (pad_mode_n < PAD_MAX) {
        pad_mode[pad_mode_n].off = (uint16_t)((p << 5) + 0x6u + (pin << 1));
        pad_mode[pad_mode_n].val = (uint16_t)(mode | func);
        pad_mode_n++;
    }
    if (high) {
        pad_hi[p] |= bit;
        pad_lo[p] = (uint16_t)(pad_lo[p] & ~bit);
    } else {
        pad_lo[p] |= bit;
        pad_hi[p] = (uint16_t)(pad_hi[p] & ~bit);
    }
}

/* Keep the recorded level in step with a pad the application drives at
 * runtime. Without this the replay would put the lamp back to its boot state
 * on every long sleep, which is to say it would turn it off by itself.
 */
void atc_pad_level(unsigned port, unsigned pin, int high)
{
    unsigned p = pad_port_index(port);
    uint16_t bit = (uint16_t)(1u << pin);

    if (high) {
        pad_hi[p] |= bit;
        pad_lo[p] = (uint16_t)(pad_lo[p] & ~bit);
    } else {
        pad_lo[p] |= bit;
        pad_hi[p] = (uint16_t)(pad_hi[p] & ~bit);
    }
}

/* Change a pad's direction at runtime, the replay table included.
 *
 * Same contract as atc_pad_level(): the table has to agree with the pad, or
 * the next sleep long enough to drop the peripheral domain puts the old mode
 * back. A pad that stops being an output is also dropped from the driven
 * masks, so the replay does not re-drive a pad that is meant to float.
 */
void atc_pad_mode(unsigned port, unsigned pin, unsigned mode, unsigned func)
{
    unsigned p = pad_port_index(port);
    uint16_t off = (uint16_t)((p << 5) + 0x6u + (pin << 1));
    uint16_t val = (uint16_t)(mode | func);
    uint8_t i;

    for (i = 0; i < pad_mode_n; i++) {
        if (pad_mode[i].off == off) {
            pad_mode[i].val = val;
            break;
        }
    }
    if (i == pad_mode_n && pad_mode_n < PAD_MAX) {
        pad_mode[pad_mode_n].off = off;
        pad_mode[pad_mode_n].val = val;
        pad_mode_n++;
    }
    SetWord16(GPIO_BASE + off, val);
    if (mode != (unsigned)OUTPUT) {
        uint16_t bit = (uint16_t)(1u << pin);

        pad_hi[p] = (uint16_t)(pad_hi[p] & ~bit);
        pad_lo[p] = (uint16_t)(pad_lo[p] & ~bit);
    }
}

static void pad_configure(GPIO_PORT port, GPIO_PIN pin, GPIO_PUPD mode,
                          GPIO_FUNCTION func, bool high)
{
    GPIO_ConfigurePin(port, pin, mode, func, high);
    pad_record((unsigned)port, (unsigned)pin, (unsigned)mode, (unsigned)func, high);
}

/* The whole pad state as straight stores. This is the wake path. */
static void pad_replay(void)
{
    uint8_t i;
    unsigned p;

    for (i = 0; i < pad_mode_n; i++) {
        SetWord16(GPIO_BASE + pad_mode[i].off, pad_mode[i].val);
    }
    for (p = 0; p < 5u; p++) {
        if (pad_hi[p] != 0u) {
            SetWord16(GPIO_BASE + (p << 5) + 2u, pad_hi[p]);
        }
        if (pad_lo[p] != 0u) {
            SetWord16(GPIO_BASE + (p << 5) + 4u, pad_lo[p]);
        }
    }
}

void GPIO_reservations(void)
{
    RESERVE_GPIO(FLASH_CLK, FLASH_CLK_PORT, FLASH_CLK_PIN, PID_SPI_CLK);
    RESERVE_GPIO(FLASH_CS, FLASH_CS_PORT, FLASH_CS_PIN, PID_SPI_EN);
    RESERVE_GPIO(FLASH_MISO, FLASH_MISO_PORT, FLASH_MISO_PIN, PID_SPI_DI);
    RESERVE_GPIO(FLASH_MOSI, FLASH_MOSI_PORT, FLASH_MOSI_PIN, PID_SPI_DO);
    RESERVE_GPIO(EPD_SCLK, EPD_SCLK_PORT, EPD_SCLK_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_DC, EPD_DC_PORT, EPD_DC_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_RST, EPD_RST_PORT, EPD_RST_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_BUSY, EPD_BUSY_PORT, EPD_BUSY_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_MOSI, EPD_MOSI_PORT, EPD_MOSI_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_CS, EPD_CS_PORT, EPD_CS_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_BS, EPD_BS_PORT, EPD_BS_PIN, PID_GPIO);
    RESERVE_GPIO(EPD_PWR, EPD_PWR_PORT, EPD_PWR_PIN, PID_GPIO);
    RESERVE_GPIO(LED_R, LED_R_PORT, LED_R_PIN, PID_GPIO);
    RESERVE_GPIO(LED_G, LED_G_PORT, LED_G_PIN, PID_GPIO);
    RESERVE_GPIO(LED_B, LED_B_PORT, LED_B_PIN, PID_GPIO);
    RESERVE_GPIO(LED_Y, LED_Y_PORT, LED_Y_PIN, PID_GPIO);
    RESERVE_GPIO(LED_A, LED_A_PORT, LED_A_PIN, PID_GPIO);
    RESERVE_GPIO(LED_C, LED_C_PORT, LED_C_PIN, PID_GPIO);
    RESERVE_GPIO(LED_Z, LED_Z_PORT, LED_Z_PIN, PID_GPIO);
    RESERVE_GPIO(NFC_SDA, NFC_SDA_PORT, NFC_SDA_PIN, PID_GPIO);
    RESERVE_GPIO(NFC_SCL, NFC_SCL_PORT, NFC_SCL_PIN, PID_GPIO);
    RESERVE_GPIO(NFC_CS, NFC_CS_PORT, NFC_CS_PIN, PID_GPIO);
    RESERVE_GPIO(NFC_IRQ, NFC_IRQ_PORT, NFC_IRQ_PIN, PID_GPIO);
}

void set_pad_functions(void)
{
    pad_mode_n = 0;
    pad_hi[0] = pad_hi[1] = pad_hi[2] = pad_hi[3] = pad_hi[4] = 0;
    pad_lo[0] = pad_lo[1] = pad_lo[2] = pad_lo[3] = pad_lo[4] = 0;

    pad_configure(FLASH_CLK_PORT, FLASH_CLK_PIN, OUTPUT, PID_SPI_CLK, false);
    pad_configure(FLASH_CS_PORT, FLASH_CS_PIN, OUTPUT, PID_SPI_EN, true);
    pad_configure(FLASH_MOSI_PORT, FLASH_MOSI_PIN, OUTPUT, PID_SPI_DO, false);
    pad_configure(FLASH_MISO_PORT, FLASH_MISO_PIN, INPUT, PID_SPI_DI, false);

    pad_configure(EPD_SCLK_PORT, EPD_SCLK_PIN, OUTPUT, PID_GPIO, false);
    pad_configure(EPD_MOSI_PORT, EPD_MOSI_PIN, OUTPUT, PID_GPIO, false);
    pad_configure(EPD_CS_PORT, EPD_CS_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(EPD_DC_PORT, EPD_DC_PIN, OUTPUT, PID_GPIO, false);
    pad_configure(EPD_RST_PORT, EPD_RST_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(EPD_BS_PORT, EPD_BS_PIN, OUTPUT, PID_GPIO, false);
    pad_configure(EPD_PWR_PORT, EPD_PWR_PIN, OUTPUT, PID_GPIO, false);
    pad_configure(EPD_BUSY_PORT, EPD_BUSY_PIN, INPUT_PULLDOWN, PID_GPIO, false);

    /* High is off for a common-anode lamp. Every unused pad is driven, so the
     * reset pull-down cannot keep the lamp lit.
     */
    pad_configure(LED_R_PORT, LED_R_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_G_PORT, LED_G_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_B_PORT, LED_B_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_Y_PORT, LED_Y_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_A_PORT, LED_A_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_C_PORT, LED_C_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(LED_Z_PORT, LED_Z_PIN, OUTPUT, PID_GPIO, true);

    /* The NFC bus idles with the chip handed to its radio side: CS high,
     * both lines let go to their pull-ups, IRQ listened to and never
     * driven. Driving SDA and SCL is what kept the chip silent before.
     */
    pad_configure(NFC_CS_PORT, NFC_CS_PIN, OUTPUT, PID_GPIO, true);
    pad_configure(NFC_SDA_PORT, NFC_SDA_PIN, INPUT, PID_GPIO, false);
    pad_configure(NFC_SCL_PORT, NFC_SCL_PIN, INPUT, PID_GPIO, false);
    pad_configure(NFC_IRQ_PORT, NFC_IRQ_PIN, INPUT, PID_GPIO, false);
}

void periph_init(void)
{
    /* Power the peripheral domain up. FIRST, and not optional.
     *
     * This was missing, and on a cold boot it did not show: PD_PER is already
     * up when the application starts, so configuring pads worked and the
     * flash answered. On a wake from extended sleep it is the whole ball
     * game. The stack calls periph_init() from BLE_WAKEUP_LP_Handler exactly
     * because the domain is down - `if (GetBits16(SYS_STAT_REG,
     * PER_IS_DOWN)) periph_init();` - and expects it to come back up. Ours
     * went straight to the pads, so the domain stayed off and every
     * peripheral register access after it was dropped on write and read back
     * as zero.
     *
     * The handler then runs the XTAL16M trim, which polls CLK_REF_VAL at
     * 0x50001604 - in that domain. Measured on the bench while wedged:
     * SYS_STAT 0x00E5 with PER_IS_DOWN set, CLK_REF_CNT 0 where 255 had just
     * been written, CLK_REF_VAL 0. The trim's `while (cnt > 1000)` can never
     * exit against a register that is always zero, so the wake ISR spun until
     * the watchdog reset the chip - about 2.6 s, which is the 27-second reboot
     * cycle this firmware had with sleep enabled, and the reason short sleeps
     * were fine. A short sleep leaves the system on XTAL16, the trim branch is
     * gated on SYS_CLK_SEL != 0 and is skipped entirely.
     *
     * The bias-sharing bit is the SDK template's companion to this and
     * belongs in the same place.
     */
    SetBits16(PMU_CTRL_REG, PERIPH_SLEEP, 0);
    while ((GetWord16(SYS_STAT_REG) & PER_IS_UP) == 0) {
    }
    SetBits16(CLK_16M_REG, XTAL16_BIAS_SH_ENABLE, 1);

    SetBits16(CLK_PER_REG, SPI_ENABLE, 1);

    if (pads_built) {
        /* The wake path. Replay the pads with the latch still closed, then
         * open it, so the pins never pass through an intermediate state on
         * the way back up.
         */
        pad_replay();
        GPIO_set_pad_latch_en(true);
        return;
    }

    /* The cold boot. A closed pad latch keeps later pin writes off the glass
     * and the flash.
     */
    GPIO_set_pad_latch_en(true);
    set_pad_functions();
    pads_built = 1;
    /* The pads and the SPI clock are up, so the flash can be read - and this
     * is the last moment before the link layer takes its address and the
     * application takes its geometry. See atc_early_init(). Cold boot only:
     * it must never run inside the wake sequence.
     */
    atc_early_init();
    /* The NFC bus on a board that has no chip on it.
     *
     * Which board this is cannot be known any earlier: the config comes
     * out of the flash, and the flash cannot be read until the pads and
     * the SPI clock are up. So all four pads are built ready for a chip
     * above, and the three that idle as inputs are left with nothing on
     * the other end on a board without one - a floating CMOS input is
     * undefined and not free. Driven high is where they were before this
     * bus existed: they were in the lamp set, and an unknown active-low
     * lamp is dark that way.
     */
    if (!atc_panel_has(ATC_CAP_NFC)) {
        pad_configure(NFC_CS_PORT, NFC_CS_PIN, OUTPUT, PID_GPIO, true);
        pad_configure(NFC_SDA_PORT, NFC_SDA_PIN, OUTPUT, PID_GPIO, true);
        pad_configure(NFC_SCL_PORT, NFC_SCL_PIN, OUTPUT, PID_GPIO, true);
        pad_configure(NFC_IRQ_PORT, NFC_IRQ_PIN, OUTPUT, PID_GPIO, true);
    }
}
