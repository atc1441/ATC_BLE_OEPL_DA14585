#include "rtt.h"

#include <stdint.h>

/* Public SEGGER RTT block. Flags 0 is skip mode, so a full buffer never stalls the radio.
 * The id is finished last so a half-built block is not matched.
 */

typedef struct {
    const char *name;
    uint8_t *buffer;
    uint32_t size;
    uint32_t write_off;
    volatile uint32_t read_off;
    uint32_t flags;
} rtt_buffer;

typedef struct {
    char id[16];
    int up_count;
    int down_count;
    rtt_buffer up[1];
    rtt_buffer down[1];
} rtt_control;

static uint8_t up_storage[1024];
static uint8_t down_storage[16];

rtt_control _SEGGER_RTT;

/* The BLE log ring. head == tail is empty, so one byte of the array is never
 * used and the usable depth is LOG_SIZE - 1.
 *
 * 256 bytes, not more: the stream drains every 100 ms and carries 200 bytes a
 * go, so this only has to cover one drain interval. RAM is the scarce thing
 * here - the whole image lives in 96 KiB of SysRAM and what is left between
 * the heap and the stack is counted in hundreds of bytes.
 */
#define LOG_SIZE 256u

static uint8_t log_buf[LOG_SIZE];
static uint16_t log_head;   /* next write */
static uint16_t log_tail;   /* next read */
static uint16_t log_lost;
static uint8_t log_on;

static uint16_t log_next(uint16_t at)
{
    at++;
    if (at >= (uint16_t)LOG_SIZE) {
        at = 0;
    }
    return at;
}

static void log_push(const uint8_t *bytes, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        uint16_t next = log_next(log_head);

        if (next == log_tail) {
            /* Full. Drop the oldest byte rather than this one: the line worth
             * reading is the one the tag printed last, and a ring that
             * refused new bytes would freeze the stream at the moment things
             * started going wrong.
             */
            log_tail = log_next(log_tail);
            if (log_lost != 0xFFFFu) {
                log_lost++;
            }
        }
        log_buf[log_head] = bytes[i];
        log_head = next;
    }
}

void rtt_log_enable(int on)
{
    if (on) {
        log_on = 1;
        return;
    }
    log_on = 0;
    /* Nobody is listening, so nothing is kept. Dropping what is left also
     * means the next client starts at the tag's present and not at whatever
     * the last one failed to collect.
     */
    log_head = 0;
    log_tail = 0;
    log_lost = 0;
}

int rtt_log_enabled(void)
{
    return log_on != 0u;
}

uint16_t rtt_log_pending(void)
{
    if (log_head >= log_tail) {
        return (uint16_t)(log_head - log_tail);
    }
    return (uint16_t)((uint16_t)LOG_SIZE - (uint16_t)(log_tail - log_head));
}

uint16_t rtt_log_take(uint8_t *dst, uint16_t cap)
{
    uint16_t n = 0;

    if (dst == 0) {
        return 0;
    }
    while (n < cap && log_tail != log_head) {
        dst[n] = log_buf[log_tail];
        log_tail = log_next(log_tail);
        n++;
    }
    return n;
}

uint16_t rtt_log_dropped(void)
{
    uint16_t n = log_lost;

    log_lost = 0;
    return n;
}

static void rtt_store(const uint8_t *bytes, uint32_t length)
{
    rtt_buffer *up = &_SEGGER_RTT.up[0];
    uint32_t write = up->write_off;
    uint32_t i;

    if (log_on) {
        log_push(bytes, length);
    }
    for (i = 0; i < length; i++) {
        uint32_t next = write + 1u;
        if (next >= up->size) {
            next = 0;
        }
        if (next == up->read_off) {
            return;
        }
        up->buffer[write] = bytes[i];
        write = next;
    }
    up->write_off = write;
}

void rtt_init(void)
{
    static const char name[] = "ATC";
    uint32_t i;

    _SEGGER_RTT.up_count = 1;
    _SEGGER_RTT.down_count = 1;
    _SEGGER_RTT.up[0].name = name;
    _SEGGER_RTT.up[0].buffer = up_storage;
    _SEGGER_RTT.up[0].size = sizeof up_storage;
    _SEGGER_RTT.up[0].write_off = 0;
    _SEGGER_RTT.up[0].read_off = 0;
    _SEGGER_RTT.up[0].flags = 0;
    _SEGGER_RTT.down[0].name = name;
    _SEGGER_RTT.down[0].buffer = down_storage;
    _SEGGER_RTT.down[0].size = sizeof down_storage;
    _SEGGER_RTT.down[0].write_off = 0;
    _SEGGER_RTT.down[0].read_off = 0;
    _SEGGER_RTT.down[0].flags = 0;
    for (i = 0; i < sizeof _SEGGER_RTT.id; i++) {
        _SEGGER_RTT.id[i] = 0;
    }
    _SEGGER_RTT.id[0] = 'S';
    _SEGGER_RTT.id[1] = 'E';
    _SEGGER_RTT.id[2] = 'G';
    _SEGGER_RTT.id[3] = 'G';
    _SEGGER_RTT.id[4] = 'E';
    _SEGGER_RTT.id[5] = 'R';
    _SEGGER_RTT.id[6] = ' ';
    _SEGGER_RTT.id[7] = 'R';
    _SEGGER_RTT.id[8] = 'T';
    _SEGGER_RTT.id[9] = 'T';
}

void rtt_puts(const char *text)
{
    uint32_t length = 0;

    if (text == 0 || _SEGGER_RTT.id[0] == 0) {
        return;
    }
    while (text[length] != 0) {
        length++;
    }
    rtt_store((const uint8_t *)text, length);
}

void rtt_hex16(uint16_t value)
{
    char out[4];
    int nibble;

    for (nibble = 3; nibble >= 0; nibble--) {
        uint8_t digit = (uint8_t)((value >> (nibble * 4)) & 0x0Fu);
        out[3 - nibble] = (char)(digit < 10 ? '0' + digit : 'A' + (digit - 10));
    }
    rtt_store((const uint8_t *)out, 4);
}

void rtt_u32(uint32_t value)
{
    char out[10];
    int pos = 10;

    if (value == 0) {
        rtt_puts("0");
        return;
    }
    while (value != 0 && pos > 0) {
        pos--;
        out[pos] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    rtt_store((const uint8_t *)(out + pos), (uint32_t)(10 - pos));
}
