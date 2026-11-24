#include "epd.h"
#include "panel.h"

#include "arch.h"
#include "arch_wdg.h"
#include "gpio.h"
#include "rtt.h"
#include "spi_flash.h"
#include "user_periph_setup.h"

/* One scanline. 128 bytes is 1024 pixels. This glass uses 110.
 * A wider glass of the same driver raises this constant.
 */
#define ATC_ROW_MAX 128u
#define EPD_CHUNK   16u

/* Data-entry mode 0x19 writes the rows from the last gate down to gate 0.
 * Width and height come from the selected row in panel.c.
 */
static const atc_panel *glass;
static uint8_t line[ATC_ROW_MAX];

/* GPIO_SetActive() checks a 64-bit reservation mask on every call. Clocking a
 * full plane that way holds the kernel off long enough for it to run out of
 * message memory. These pads are already reserved; the data registers are the
 * same ones the GPIO driver writes.
 */
#define PORT_DATA(port) (*(volatile uint16_t *)(0x50003000u + ((uint32_t)(port) << 5)))
#define PORT_SET(port)  (*(volatile uint16_t *)(0x50003002u + ((uint32_t)(port) << 5)))
#define PORT_CLR(port)  (*(volatile uint16_t *)(0x50003004u + ((uint32_t)(port) << 5)))

enum {
    PH_PWR = 0,
    PH_RST_LOW,
    PH_RST_HIGH,
    PH_IDLE,
    PH_SOFT,
    PH_T46,
    PH_T47,
    PH_INIT,
    PH_BLACK,
    PH_RED,
    PH_DRAW,
    PH_RISE,
    PH_FALL,
    PH_HOLD
};

static uint8_t phase;
static uint8_t running;
static uint8_t img_type;
static uint8_t read_warned;
static uint16_t left;
static uint32_t flash_base;
static uint32_t row;
static epd_row_fn draw_black;

static uint16_t glass_w(void)
{
    return glass->width;
}

static uint16_t glass_h(void)
{
    return glass->height;
}

static uint16_t glass_row(void)
{
    return (uint16_t)(glass->width >> 3);
}

static uint16_t glass_y1(void)
{
    return (uint16_t)(glass->height - 1u);
}

static int glass_ok(const atc_panel *panel)
{
    return panel != 0
        && panel->driver == ATC_DRV_ZKC75S
        && panel->width >= 8u
        && (panel->width & 7u) == 0u
        && (panel->width >> 3) <= ATC_ROW_MAX
        && panel->height != 0u;
}

/* Set while the panel rail is up and the controller is awake. */
static uint8_t powered;

static void pin_high(unsigned port, unsigned pin)
{
    PORT_SET(port) = (uint16_t)(1u << pin);
}

static void pin_low(unsigned port, unsigned pin)
{
    PORT_CLR(port) = (uint16_t)(1u << pin);
}

static int busy_high(void)
{
    return (PORT_DATA(EPD_BUSY_PORT) & (uint16_t)(1u << EPD_BUSY_PIN)) != 0;
}

static void spi_byte(uint8_t value)
{
    unsigned bit;

    for (bit = 0; bit < 8u; bit++) {
        if (value & 0x80u) {
            pin_high(EPD_MOSI_PORT, EPD_MOSI_PIN);
        } else {
            pin_low(EPD_MOSI_PORT, EPD_MOSI_PIN);
        }
        pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
        pin_high(EPD_SCLK_PORT, EPD_SCLK_PIN);
        value <<= 1;
    }
    pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
}

static void write_cmd(uint8_t command)
{
    pin_low(EPD_DC_PORT, EPD_DC_PIN);
    pin_low(EPD_CS_PORT, EPD_CS_PIN);
    spi_byte(command);
    pin_high(EPD_CS_PORT, EPD_CS_PIN);
}

static void write_data(uint8_t data)
{
    pin_high(EPD_DC_PORT, EPD_DC_PIN);
    pin_low(EPD_CS_PORT, EPD_CS_PIN);
    spi_byte(data);
    pin_high(EPD_CS_PORT, EPD_CS_PIN);
}

static void write_u16(uint16_t value)
{
    write_data((uint8_t)(value & 0xFFu));
    write_data((uint8_t)(value >> 8));
}

static void open_plane(uint8_t command, const char *label)
{
    write_cmd(0x4E);
    write_u16(0);
    write_cmd(0x4F);
    write_u16(glass_y1());
    write_cmd(command);
    rtt_puts(label);
}

static void send_chunk(uint32_t address, int invert, int zeros)
{
    uint32_t n = EPD_CHUNK;
    uint16_t nbytes = glass_row();

    if (row + n > glass_h()) {
        n = glass_h() - row;
    }
    while (n--) {
        uint32_t got = 0;
        uint32_t src;
        uint32_t i;

        /* The last gate, written first, is the controller edge of this glass.
         * An OEPL row is stored with row 0 at the far edge, so that row is
         * sent last.
         */
        src = (glass_h() - 1u - row) * nbytes;
        if (zeros) {
            for (i = 0; i < nbytes; i++) {
                line[i] = 0;
            }
        } else if (draw_black) {
            draw_black((uint16_t)(glass_h() - 1u - row), line, nbytes);
        } else if (spi_flash_read_data(line, address + src, nbytes, &got) != SPI_FLASH_ERR_OK
                   || got != nbytes) {
            if (!read_warned) {
                read_warned = 1;
                rtt_puts("plane read fail\n");
            }
            for (i = 0; i < nbytes; i++) {
                line[i] = invert ? 0x00u : 0xFFu;
            }
        }
        for (i = 0; i < nbytes; i++) {
            write_data(invert ? (uint8_t)~line[i] : line[i]);
        }
        row++;
    }
}

static int timed(void)
{
    if (left > 0u) {
        left--;
        if (left > 0u) {
            return 1;
        }
    }
    return 0;
}

static void panel_sleep(void);

/* Both ends of a refresh go through here, and both used to do nothing but
 * clear the flag - which left the panel rail switched on and the controller
 * awake until something else happened to turn it off. On this board that was
 * the better part of a milliamp, held until the next temperature read, and
 * it is what made the current after an image upload look like a leak.
 */
static int finish_ok(void)
{
    rtt_puts("refresh busy\n");
    running = 0;
    panel_sleep();
    return 0;
}

static int finish_fail(void)
{
    running = 0;
    panel_sleep();
    return -1;
}

void epd_stop(void)
{
    running = 0;
    draw_black = 0;
    panel_sleep();
}

void epd_begin(uint32_t base, uint8_t type)
{
    const atc_panel *panel = atc_panel_get();

    if (!glass_ok(panel)) {
        running = 0;
        return;
    }
    glass = panel;
    flash_base = base;
    img_type = type;
    draw_black = 0;
    row = 0;
    read_warned = 0;
    running = 1;
    pin_low(EPD_BS_PORT, EPD_BS_PIN);
    pin_high(EPD_CS_PORT, EPD_CS_PIN);
    pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
    pin_high(EPD_PWR_PORT, EPD_PWR_PIN);
    atc_pad_level(EPD_PWR_PORT, EPD_PWR_PIN, 1);
    powered = 1;
    pin_high(EPD_RST_PORT, EPD_RST_PIN);
    phase = PH_PWR;
    left = 4u;
}

void epd_begin_draw(epd_row_fn black)
{
    epd_begin(0, 0x20u);
    draw_black = black;
}

int epd_step(void)
{
    if (!running) {
        return -1;
    }
    wdg_reload(WATCHDOG_DEFAULT_PERIOD);

    switch (phase) {
    case PH_PWR:
    case PH_RST_LOW:
    case PH_RST_HIGH:
        if (timed()) {
            return 1;
        }
        if (phase == PH_PWR) {
            pin_low(EPD_RST_PORT, EPD_RST_PIN);
            phase = PH_RST_LOW;
            left = 1u;
            return 1;
        }
        if (phase == PH_RST_LOW) {
            pin_high(EPD_RST_PORT, EPD_RST_PIN);
            phase = PH_RST_HIGH;
            left = 2u;
            return 1;
        }
        phase = PH_IDLE;
        left = 200u;
        return 1;

    case PH_IDLE:
        if (busy_high() && left > 0u) {
            left--;
            return 1;
        }
        write_cmd(0x12);
        phase = PH_SOFT;
        left = 200u;
        return 1;

    case PH_SOFT:
        if (busy_high() && left > 0u) {
            left--;
            return 1;
        }
        write_cmd(0x46);
        write_data(0xF7);
        phase = PH_T46;
        left = 200u;
        return 1;

    case PH_T46:
        if (busy_high() && left > 0u) {
            left--;
            return 1;
        }
        write_cmd(0x47);
        write_data(0xF7);
        phase = PH_T47;
        left = 200u;
        return 1;

    case PH_T47:
        if (busy_high() && left > 0u) {
            left--;
            return 1;
        }
        write_cmd(0x0C);
        write_data(0xAE);
        write_data(0xC7);
        write_data(0xC3);
        write_data(0xC0);
        write_data(0x40);
        write_cmd(0x01);
        write_u16(glass_y1());
        write_data(0x00);
        write_cmd(0x11);
        write_data(0x01);
        write_cmd(0x44);
        write_u16(0);
        write_u16((uint16_t)(glass_w() - 1u));
        write_cmd(0x45);
        write_u16(glass_y1());
        write_u16(0);
        write_cmd(0x3C);
        write_data(0x01);
        write_cmd(0x18);
        write_data(0x80);
        write_cmd(0x22);
        write_data(0xB1);
        write_cmd(0x20);
        phase = PH_INIT;
        left = 200u;
        return 1;

    case PH_INIT:
        if (busy_high() && left > 0u) {
            left--;
            return 1;
        }
        row = 0;
        phase = PH_BLACK;
        return 1;

    case PH_BLACK:
        if (row == 0u) {
            open_plane(0x24, "refresh black\n");
        }
        send_chunk(flash_base, 1, 0);
        if ((row & 127u) == 0u) {
            rtt_puts("row ");
            rtt_u32(row);
            rtt_puts("\n");
        }
        if (row >= glass_h()) {
            row = 0;
            phase = PH_RED;
        }
        return 1;

    case PH_RED: {
        uint32_t plane = ((uint32_t)glass_w() * glass_h()) / 8u;

        if (row == 0u) {
            open_plane(0x26, "refresh red\n");
        }
        if (img_type == 0x21u) {
            send_chunk(flash_base + plane, 0, 0);
        } else {
            send_chunk(0, 0, 1);
        }
        if ((row & 127u) == 0u) {
            rtt_puts("row ");
            rtt_u32(row);
            rtt_puts("\n");
        }
        if (row >= glass_h()) {
            phase = PH_DRAW;
        }
        return 1;
    }

    case PH_DRAW:
        rtt_puts("refresh draw\n");
        write_cmd(0x22);
        write_data(0xC7);
        write_cmd(0x20);
        phase = PH_RISE;
        left = 300u;
        return 1;

    case PH_RISE:
        if (busy_high()) {
            rtt_puts("refresh rise\n");
            phase = PH_FALL;
            left = 4000u;
            return 1;
        }
        if (left == 0u) {
            rtt_puts("refresh nobusy\n");
            phase = PH_HOLD;
            left = 2000u;
            return 1;
        }
        left--;
        return 1;

    case PH_FALL:
        if (!busy_high()) {
            return finish_ok();
        }
        if (left == 0u) {
            rtt_puts("refresh timeout\n");
            return finish_fail();
        }
        left--;
        return 1;

    case PH_HOLD:
        if (busy_high()) {
            rtt_puts("refresh rise\n");
            phase = PH_FALL;
            left = 4000u;
            return 1;
        }
        if (left == 0u) {
            return finish_fail();
        }
        left--;
        return 1;

    default:
        return finish_fail();
    }
}

/* 12-bit temperature, then 4 dummy bits. Degrees are the high byte.
 * 25.5 C is the word 0x1980. A zero dummy nibble is required so a floating
 * pad is not reported as a room temperature.
 */
static int celsius_of(uint16_t word, int8_t *out)
{
    int16_t hi;

    if (word == 0 || word == 0xFFFFu || (word & 0x000Fu) != 0u) {
        return -1;
    }
    hi = (int16_t)((int16_t)word >> 8);
    if (hi < -20 || hi > 70 || hi == 0) {
        return -1;
    }
    *out = (int8_t)hi;
    return 0;
}

static int level(unsigned port, unsigned pin)
{
    return (int)((PORT_DATA(port) >> pin) & 1u);
}

static void wait_us(uint32_t usec)
{
    while (usec > 1000u) {
        wdg_reload(WATCHDOG_DEFAULT_PERIOD);
        arch_asm_delay_us(1000);
        usec -= 1000u;
    }
    if (usec != 0u) {
        arch_asm_delay_us((int)usec);
    }
}

static void wait_pulse(void)
{
    uint32_t i;

    for (i = 0; i < 30u; i++) {
        wdg_reload(WATCHDOG_DEFAULT_PERIOD);
        if (busy_high()) {
            break;
        }
        wait_us(1000);
    }
    for (i = 0; i < 100u; i++) {
        wdg_reload(WATCHDOG_DEFAULT_PERIOD);
        if (!busy_high()) {
            return;
        }
        wait_us(1000);
    }
}

static uint16_t shift_in(unsigned port, unsigned pin)
{
    uint16_t word = 0;
    unsigned bit;

    /* The panel samples and shifts on the rising edge, same as spi_byte. */
    for (bit = 0; bit < 16u; bit++) {
        pin_high(EPD_SCLK_PORT, EPD_SCLK_PIN);
        word = (uint16_t)((word << 1) | (uint16_t)level(port, pin));
        pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
    }
    return word;
}

static void pad_input(unsigned port, unsigned pin)
{
    GPIO_ConfigurePin((GPIO_PORT)port, (GPIO_PIN)pin, INPUT, PID_GPIO, false);
}

static uint16_t read_reg16(unsigned port, unsigned pin)
{
    uint16_t word;

    GPIO_ConfigurePin(EPD_MOSI_PORT, EPD_MOSI_PIN, OUTPUT, PID_GPIO, false);
    pin_low(EPD_DC_PORT, EPD_DC_PIN);
    pin_low(EPD_CS_PORT, EPD_CS_PIN);
    spi_byte(0x1B);
    pin_high(EPD_DC_PORT, EPD_DC_PIN);
    pad_input(port, pin);
    word = shift_in(port, pin);
    pin_high(EPD_CS_PORT, EPD_CS_PIN);
    return word;
}

static void panel_quiet(void);

/* Put the controller to sleep, then take the rail away.
 *
 * Command 0x10 with data 0x01 is the SSD16xx deep sleep. Without it the
 * controller sits in standby with its regulators running; the glass keeps its
 * image either way, so there is nothing to lose by sending it. It only lands
 * while the panel is still powered, which is why this runs before EPD_PWR
 * goes low, and RST stays high afterwards - a reset pulse is exactly what
 * wakes the controller up again, and epd_begin() issues one when a refresh
 * really is wanted.
 */
static void panel_sleep(void)
{
    if (!powered) {
        return;
    }
    write_cmd(0x10);
    write_data(0x01);
    wait_us(2000);
    panel_quiet();
}

/* Every pad low, and the rail last.
 *
 * CS and RST used to be left high here, which is the natural idle state for
 * a panel that is powered - and the wrong one for a panel that is not. With
 * the rail down, a pad held at 3.3 V drives current through the controller's
 * input protection into its own dead supply, and on this board that was
 * measured at about 340 uA: more than twice everything else on the tag put
 * together. Low costs nothing, and the glass keeps its image regardless.
 *
 * The levels are recorded as well as driven. periph_init() replays the table
 * after any sleep long enough to drop the peripheral domain, so a pad that is
 * only driven and not recorded comes back high on the next wake - which is
 * to say the leak would return a couple of seconds later.
 */
static void panel_quiet(void)
{
    pin_low(EPD_CS_PORT, EPD_CS_PIN);
    pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
    pin_low(EPD_DC_PORT, EPD_DC_PIN);
    GPIO_ConfigurePin(EPD_MOSI_PORT, EPD_MOSI_PIN, OUTPUT, PID_GPIO, false);
    pin_low(EPD_RST_PORT, EPD_RST_PIN);
    pin_low(EPD_BS_PORT, EPD_BS_PIN);
    pin_low(EPD_PWR_PORT, EPD_PWR_PIN);
    atc_pad_level(EPD_CS_PORT, EPD_CS_PIN, 0);
    atc_pad_level(EPD_SCLK_PORT, EPD_SCLK_PIN, 0);
    atc_pad_level(EPD_DC_PORT, EPD_DC_PIN, 0);
    atc_pad_level(EPD_MOSI_PORT, EPD_MOSI_PIN, 0);
    atc_pad_level(EPD_RST_PORT, EPD_RST_PIN, 0);
    atc_pad_level(EPD_BS_PORT, EPD_BS_PIN, 0);
    atc_pad_level(EPD_PWR_PORT, EPD_PWR_PIN, 0);
    powered = 0;
}

int epd_read_celsius(int8_t *celsius)
{
    uint16_t word;
    int8_t found;

    if (celsius == 0 || running) {
        return -1;
    }
    pin_low(EPD_BS_PORT, EPD_BS_PIN);
    pin_high(EPD_CS_PORT, EPD_CS_PIN);
    pin_low(EPD_SCLK_PORT, EPD_SCLK_PIN);
    pin_high(EPD_PWR_PORT, EPD_PWR_PIN);
    atc_pad_level(EPD_PWR_PORT, EPD_PWR_PIN, 1);
    powered = 1;
    pin_high(EPD_RST_PORT, EPD_RST_PIN);
    wait_us(20000);
    pin_low(EPD_RST_PORT, EPD_RST_PIN);
    wait_us(10000);
    pin_high(EPD_RST_PORT, EPD_RST_PIN);
    wait_us(20000);
    wait_pulse();

    write_cmd(0x12);
    wait_pulse();
    write_cmd(0x18);
    write_data(0x80);
    /* Clock and temperature load only. Bit 2 would start a refresh. */
    write_cmd(0x22);
    write_data(0xA0);
    write_cmd(0x20);
    wait_pulse();

    /* The controller shares one data wire. After the read command the same
     * pad shifts the temperature back. The other pads stay quiet.
     */
    word = read_reg16(EPD_MOSI_PORT, EPD_MOSI_PIN);
    rtt_puts("ts ");
    rtt_hex16(word);
    rtt_puts("\n");

    panel_quiet();
    if (celsius_of(word, &found) == 0) {
        *celsius = found;
        return 0;
    }
    return -1;
}
