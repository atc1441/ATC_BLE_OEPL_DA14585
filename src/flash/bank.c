/* Bit-bang programmer for the application bank. Erase and program stay
 * inside 0x1F000..0x38000. The bootloader and the image at 0x4000 stay put.
 */

#include <stdint.h>

#define REG16(addr) (*(volatile uint16_t *)(addr))

#define P0_DATA 0x50003000u
#define P0_SET 0x50003002u
#define P0_RST 0x50003004u
#define P0_MODE 0x50003006u
#define SET_FREEZE 0x50003300u
#define WATCHDOG 0x50003100u
#define PMU_CTRL 0x50000010u
#define SYS_STAT 0x50000014u
#define CLK_PER 0x50000004u
#define SPI_CTRL 0x50001200u
#define SPI_RXTX 0x50001202u
#define SPI_CLR 0x50001206u
#define SPI_CTRL1 0x50001208u

#define PIN_CLK 0u
#define PIN_CS 3u
#define PIN_MISO 5u
#define PIN_MOSI 6u

#define MODE_OUT 0x0300u
#define MODE_IN 0x0000u

#define BANK_LO 0x0001F000u
#define BANK_HI 0x00038000u

#define CMD_JEDEC 1u
#define CMD_ERASE 2u
#define CMD_PROGRAM 3u
#define CMD_READ 4u

#define ST_OK 0xA7C00002u
#define ST_RANGE 0xA7C000E1u
#define ST_TIMEOUT 0xA7C000E2u
#define ST_CMD 0xA7C000E3u
#define ST_MAGIC 0xA7C000E4u

struct mail {
    uint32_t magic;
    uint32_t cmd;
    uint32_t addr;
    uint32_t len;
    uint32_t buf;
    uint32_t status;
    uint32_t result;
};

#define MAIL ((volatile struct mail *)0x07FD5000u)
#define MAIL_MAGIC 0xA7C0115Eu

static void pin_mode(unsigned pin, uint16_t mode)
{
    REG16(P0_MODE + pin * 2u) = mode;
}

static void pin_set(unsigned pin, int high)
{
    REG16(high ? P0_SET : P0_RST) = (uint16_t)(1u << pin);
}

static int pin_get(unsigned pin)
{
    return (REG16(P0_DATA) >> pin) & 1;
}

static void pause(void)
{
    volatile uint32_t n = 200u;
    while (n--) {
    }
}

static int spi_fail;

static uint8_t xfer(uint8_t value)
{
    uint32_t spins = 0;

    REG16(SPI_RXTX) = value;
    while ((REG16(SPI_CTRL) & 0x2000u) == 0) {
        if (++spins > 20000u) {
            spi_fail = 1;
            return 0;
        }
    }
    value = (uint8_t)REG16(SPI_RXTX);
    REG16(SPI_CLR) = 1;
    return value;
}

static void periph_on(void)
{
    uint16_t pmu = REG16(PMU_CTRL);
    uint32_t spins = 0;

    pmu &= (uint16_t)~0x0002u;
    REG16(PMU_CTRL) = pmu;
    /* Pads ignore the mode registers until this latch is open. */
    REG16(0x50000012u) = (uint16_t)(REG16(0x50000012u) | 0x0020u);
    while ((REG16(SYS_STAT) & 0x0008u) == 0) {
        if (++spins > 100000u) {
            break;
        }
    }
}

static void spi_hw_init(int mode3)
{
    uint16_t clk = REG16(CLK_PER);
    uint16_t ctrl = (uint16_t)((mode3 ? 3u : 0u) << 1) | (3u << 3);

    clk |= 0x0800u;
    REG16(CLK_PER) = clk;
    pin_mode(PIN_CLK, (uint16_t)(MODE_OUT | 7u));
    pin_mode(PIN_MOSI, (uint16_t)(MODE_OUT | 6u));
    pin_mode(PIN_MISO, (uint16_t)(MODE_IN | 5u));
    pin_mode(PIN_CS, MODE_OUT);
    pin_set(PIN_CS, 1);
    REG16(SPI_CTRL) = ctrl;
    REG16(SPI_CTRL1) = 0;
    REG16(SPI_CLR) = 1;
    REG16(SPI_CTRL) = (uint16_t)(ctrl | 1u);
    spi_fail = 0;
}

static void cs_begin(void)
{
    pin_set(PIN_CS, 0);
    pause();
}

static void cs_end(void)
{
    pause();
    pin_set(PIN_CS, 1);
    pause();
}

static void cmd_addr(uint8_t cmd, uint32_t addr)
{
    xfer(cmd);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

static void wren(void)
{
    cs_begin();
    xfer(0x06);
    cs_end();
}

static int wait_ready(void)
{
    uint32_t spins = 0;

    for (;;) {
        uint8_t status;
        cs_begin();
        xfer(0x05);
        status = xfer(0x00);
        cs_end();
        if (spi_fail || (status & 0x01u) == 0) {
            return spi_fail ? -1 : 0;
        }
        if (++spins > 20000u) {
            return -1;
        }
    }
}

static int in_bank(uint32_t addr, uint32_t len)
{
    if (len == 0 || addr < BANK_LO) {
        return 0;
    }
    if (addr + len < addr) {
        return 0;
    }
    return addr + len <= BANK_HI;
}

/* Reads may also cover the image at 0x4000, the pin record, and the waveform.
 * Erase and program stay inside the application bank.
 */
static int in_read(uint32_t addr, uint32_t len)
{
    if (len == 0 || len > 4096u || addr + len < addr) {
        return 0;
    }
    if (in_bank(addr, len)) {
        return 1;
    }
    if (addr >= 0x00004000u && addr + len <= 0x00014000u) {
        return 1;
    }
    if (addr >= 0x00039000u && addr + len <= 0x0003A000u) {
        return 1;
    }
    if (addr >= 0x0003B000u && addr + len <= 0x00040000u) {
        return 1;
    }
    return 0;
}

static void pins_init(void)
{
    pin_mode(PIN_CLK, MODE_OUT);
    pin_mode(PIN_CS, MODE_OUT);
    pin_mode(PIN_MOSI, MODE_OUT);
    pin_mode(PIN_MISO, MODE_IN);
    pin_set(PIN_CLK, 0);
    pin_set(PIN_MOSI, 0);
    pin_set(PIN_CS, 1);
}

static uint32_t read_jedec_once(void)
{
    uint32_t id;

    cs_begin();
    xfer(0xAB);
    cs_end();
    pause();
    if (spi_fail) {
        return 0;
    }
    cs_begin();
    xfer(0x9F);
    id = (uint32_t)xfer(0x00) << 16;
    id |= (uint32_t)xfer(0x00) << 8;
    id |= xfer(0x00);
    cs_end();
    if (spi_fail) {
        return 0;
    }
    return id;
}

static uint32_t read_jedec(void)
{
    uint32_t id;

    spi_hw_init(1);
    id = read_jedec_once();
    if (id != 0 && id != 0x00FFFFFFu) {
        return id;
    }
    spi_hw_init(0);
    return read_jedec_once();
}

__attribute__((section(".text.flash_entry"), used, noreturn)) void flash_entry(void)
{
    struct mail local;
    uint32_t i;

    REG16(WATCHDOG) = 0x00FFu;
    REG16(SET_FREEZE) = 0x0008u;
    periph_on();
    spi_hw_init(1);

    if (MAIL->magic != MAIL_MAGIC) {
        MAIL->status = ST_MAGIC;
        for (;;) {
        }
    }
    local.cmd = MAIL->cmd;
    local.addr = MAIL->addr;
    local.len = MAIL->len;
    local.buf = MAIL->buf;

    if (local.cmd == CMD_JEDEC) {
        MAIL->result = read_jedec();
        MAIL->status = (MAIL->result == 0 || MAIL->result == 0x00FFFFFFu) ? ST_TIMEOUT : ST_OK;
    } else if (local.cmd == CMD_ERASE) {
        if (!in_bank(local.addr, 0x1000u) || (local.addr & 0xFFFu) != 0) {
            MAIL->status = ST_RANGE;
        } else if (wait_ready() != 0) {
            MAIL->status = ST_TIMEOUT;
        } else {
            wren();
            cs_begin();
            cmd_addr(0x20, local.addr);
            cs_end();
            MAIL->status = wait_ready() == 0 ? ST_OK : ST_TIMEOUT;
        }
    } else if (local.cmd == CMD_PROGRAM) {
        /* Up to 4096 bytes, split into the flash's 256-byte pages here
         * rather than by the host.
         *
         * A page is all the flash takes in one program command, but one
         * page per mailbox call made the host pay a round trip for each,
         * about a second measured - most of the eight minutes a bank used
         * to take. This is the same work, in one call.
         */
        if (!in_bank(local.addr, local.len) || local.len > 4096u) {
            MAIL->status = ST_RANGE;
        } else {
            const uint8_t *src = (const uint8_t *)local.buf;
            uint32_t done = 0u;

            MAIL->status = ST_OK;
            while (done < local.len) {
                uint32_t page = 256u - ((local.addr + done) & 0xFFu);
                uint32_t left = local.len - done;

                if (page > left) {
                    page = left;
                }
                if (wait_ready() != 0) {
                    MAIL->status = ST_TIMEOUT;
                    break;
                }
                wren();
                cs_begin();
                cmd_addr(0x02, local.addr + done);
                for (i = 0; i < page; i++) {
                    xfer(src[done + i]);
                }
                cs_end();
                if (wait_ready() != 0) {
                    MAIL->status = ST_TIMEOUT;
                    break;
                }
                done += page;
            }
        }
    } else if (local.cmd == CMD_READ) {
        uint8_t *dst = (uint8_t *)local.buf;
        if (!in_read(local.addr, local.len)) {
            MAIL->status = ST_RANGE;
        } else {
            cs_begin();
            cmd_addr(0x03, local.addr);
            for (i = 0; i < local.len; i++) {
                dst[i] = xfer(0x00);
            }
            cs_end();
            MAIL->status = ST_OK;
        }
    } else {
        MAIL->status = ST_CMD;
    }

    for (;;) {
        __asm volatile("bkpt 0");
    }
}
