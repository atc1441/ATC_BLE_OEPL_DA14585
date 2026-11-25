/* Bit-banged I2C to the board's NFC chip.
 *
 * The pinout was measured on the 13.3 inch board and confirmed by the
 * board's designer: SDA and SCL carry external pull-ups, CS is active low
 * and IRQ is the chip's own output. VCC is permanently on, so CS is the
 * only thing standing between the MCU and the chip - with CS high the chip
 * ignores the bus, which is how it has to idle, because a reader cannot
 * have the chip while the MCU is holding it.
 *
 * The chip takes a 16 bit address, is byte addressable and counts on by
 * itself, and its data area begins at 0x0010. Its memory was read out and
 * agrees with what a reader sees over the air.
 *
 * Two details matter for size and for the bus. This image runs out of
 * SysRAM, so its code competes with its data: the pads are therefore
 * poked through their registers rather than through GPIO_ConfigurePin,
 * which turns every edge into a couple of stores instead of a call. And
 * open drain is done the only way these pads allow - a line is let go by
 * making the pad an input and pulled down by making it a low output - so
 * the MCU never drives against the chip.
 */
#include "nfc.h"

#include "arch.h"
#include "gpio.h"
#include "user_periph_setup.h"

#define NFC_ADDR    0x57u     /* 7 bit; the only address on this bus */
#define NFC_HALF    5         /* microseconds, so about 100 kHz */
#define NFC_SETTLE  200       /* microseconds after CS goes low */
#define NFC_POLL    40u       /* acknowledge polls after a page write */

#define MODE_INPUT  0x0000u   /* the pull-ups hold the line high */
#define MODE_OUTPUT 0x0300u

static volatile uint16_t *port_data(unsigned port)
{
    static const uint32_t base[4] = {
        GPIO_BASE, GPIO_BASE + 0x20u, GPIO_BASE + 0x40u, GPIO_BASE + 0x80u
    };

    return (volatile uint16_t *)base[port];
}

static volatile uint16_t *pad_mode(unsigned port, unsigned pin)
{
    return port_data(port) + 3u + pin;    /* the MODE registers, 16 bit each */
}

/* The set and reset registers, not a read and write of DATA: on an input
 * pad DATA reads the level on the pin, so writing it back would hand a
 * neighbouring output whatever its pin happened to be sitting at.
 */
static void pin_set(unsigned port, unsigned pin)
{
    *(port_data(port) + 1u) = (uint16_t)(1u << pin);
}

static void pin_clear(unsigned port, unsigned pin)
{
    *(port_data(port) + 2u) = (uint16_t)(1u << pin);
}

static void line_low(unsigned port, unsigned pin)
{
    pin_clear(port, pin);
    *pad_mode(port, pin) = MODE_OUTPUT;
}

static void line_free(unsigned port, unsigned pin)
{
    *pad_mode(port, pin) = MODE_INPUT;
}

static void sda_low(void)  { line_low(NFC_SDA_PORT, NFC_SDA_PIN); }
static void sda_free(void) { line_free(NFC_SDA_PORT, NFC_SDA_PIN); }
static void scl_low(void)  { line_low(NFC_SCL_PORT, NFC_SCL_PIN); }
static void scl_free(void) { line_free(NFC_SCL_PORT, NFC_SCL_PIN); }

static int sda_read(void)
{
    return (*port_data(NFC_SDA_PORT) >> NFC_SDA_PIN) & 1u;
}

static void half(void)
{
    arch_asm_delay_us(NFC_HALF);
}

static void bus_start(void)
{
    sda_free();
    scl_free();
    half();
    sda_low();
    half();
    scl_low();
    half();
}

static void bus_restart(void)
{
    sda_free();
    half();
    scl_free();
    half();
    sda_low();
    half();
    scl_low();
    half();
}

static void bus_stop(void)
{
    sda_low();
    half();
    scl_free();
    half();
    sda_free();
    half();
}

static void bit_out(unsigned one)
{
    if (one) {
        sda_free();
    } else {
        sda_low();
    }
    half();
    scl_free();
    half();
    scl_low();
    half();
}

static int bit_in(void)
{
    int bit;

    sda_free();
    half();
    scl_free();
    half();
    bit = sda_read();
    scl_low();
    half();
    return bit;
}

/* Returns 1 when the chip acknowledged. */
static int byte_out(uint8_t value)
{
    unsigned i;

    for (i = 0; i < 8u; i++) {
        bit_out((value & 0x80u) != 0u);
        value = (uint8_t)(value << 1);
    }
    return bit_in() == 0;
}

static uint8_t byte_in(int ack)
{
    uint8_t value = 0;
    unsigned i;

    for (i = 0; i < 8u; i++) {
        value = (uint8_t)((value << 1) | (unsigned)bit_in());
    }
    bit_out(ack ? 0u : 1u);
    return value;
}

/* Address the chip for a write and see whether it answers. Also how an
 * EEPROM is asked whether its last page has finished burning in.
 */
static int say_hello(void)
{
    int ok;

    bus_start();
    ok = byte_out((uint8_t)(NFC_ADDR << 1));
    bus_stop();
    return ok;
}

static int open_at(uint16_t offset, unsigned read)
{
    int ok;

    bus_start();
    ok = byte_out((uint8_t)(NFC_ADDR << 1));
    ok = ok && byte_out((uint8_t)(offset >> 8));
    ok = ok && byte_out((uint8_t)offset);
    if (ok && read) {
        bus_restart();
        ok = byte_out((uint8_t)((NFC_ADDR << 1) | 1u));
    }
    return ok;
}

int atc_nfc_begin(void)
{
    unsigned attempt;

    pin_clear(NFC_CS_PORT, NFC_CS_PIN);
    *pad_mode(NFC_CS_PORT, NFC_CS_PIN) = MODE_OUTPUT;
    atc_pad_level((unsigned)NFC_CS_PORT, (unsigned)NFC_CS_PIN, 0);
    sda_free();
    scl_free();
    arch_asm_delay_us(NFC_SETTLE);
    for (attempt = 0; attempt < 5u; attempt++) {
        if (say_hello()) {
            return 1;
        }
        arch_asm_delay_us(1000);
    }
    atc_nfc_end();
    return 0;
}

void atc_nfc_end(void)
{
    sda_free();
    scl_free();
    pin_set(NFC_CS_PORT, NFC_CS_PIN);
    *pad_mode(NFC_CS_PORT, NFC_CS_PIN) = MODE_OUTPUT;
    atc_pad_level((unsigned)NFC_CS_PORT, (unsigned)NFC_CS_PIN, 1);
}

int atc_nfc_page(uint16_t offset, const uint8_t *data, uint8_t len)
{
    unsigned i;
    int ok;

    if (len == 0u || len > ATC_NFC_PAGE) {
        return 0;
    }
    ok = open_at(offset, 0u);
    for (i = 0; ok && i < len; i++) {
        ok = byte_out(data[i]);
    }
    bus_stop();
    if (!ok) {
        return 0;
    }
    /* The page is burning in. Ask until it answers again, rather than
     * guessing at a delay.
     */
    for (i = 0; i < NFC_POLL; i++) {
        arch_asm_delay_us(500);
        if (say_hello()) {
            return 1;
        }
    }
    return 0;
}

int atc_nfc_read(uint16_t offset, uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (len == 0u || !open_at(offset, 1u)) {
        bus_stop();
        return 0;
    }
    for (i = 0; i < len; i++) {
        buf[i] = byte_in(i + 1u < len);
    }
    bus_stop();
    return 1;
}
