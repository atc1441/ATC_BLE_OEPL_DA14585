#include "settings.h"

#include <string.h>

/*
 * Record layout, 32 bytes:
 *
 *   0  u32  magic, ATC_SETTINGS_MAGIC
 *   4  u8   version
 *   5  u8   flags, bit 0 = an address is stored
 *   6  u16  config_id, little-endian
 *   8  u8   address, 8 bytes, OEPL order (0x13 0x37 then the six)
 *  16  u8   reserved, 14 bytes, zero
 *  30  u16  CRC-16/CCITT-FALSE over bytes 0..29, little-endian
 */
#define OFF_MAGIC   0u
#define OFF_VERSION 4u
#define OFF_FLAGS   5u
#define OFF_SCREEN  6u
#define OFF_MAC     8u
#define OFF_RSVD    16u
#define OFF_CRC     30u

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

/* CRC-16/CCITT-FALSE. Bitwise, because this runs twice per boot at most and a
 * 512-byte table would cost more than it saves.
 */
static uint16_t crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8u; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void atc_settings_clear(atc_settings *out)
{
    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof *out);
}

void atc_settings_encode(const atc_settings *in, uint8_t rec[ATC_SETTINGS_BYTES])
{
    if (in == 0 || rec == 0) {
        return;
    }
    memset(rec, 0, ATC_SETTINGS_BYTES);
    write_le32(rec + OFF_MAGIC, ATC_SETTINGS_MAGIC);
    rec[OFF_VERSION] = (uint8_t)ATC_SETTINGS_VERSION;
    rec[OFF_FLAGS] = (uint8_t)(in->have_mac ? ATC_SETTINGS_HAVE_MAC : 0u);
    write_le16(rec + OFF_SCREEN, in->config_id);
    memcpy(rec + OFF_MAC, in->mac, ATC_SETTINGS_MAC_BYTES);
    write_le16(rec + OFF_CRC, crc16(rec, OFF_CRC));
}

int atc_settings_decode(const uint8_t rec[ATC_SETTINGS_BYTES], atc_settings *out)
{
    atc_settings got;

    if (rec == 0 || out == 0) {
        return 0;
    }
    if (read_le32(rec + OFF_MAGIC) != ATC_SETTINGS_MAGIC) {
        return 0;
    }
    /* A newer record is not guessed at. An older build that meets a layout it
     * does not know keeps its defaults rather than reading the wrong fields.
     */
    if (rec[OFF_VERSION] != (uint8_t)ATC_SETTINGS_VERSION) {
        return 0;
    }
    if (read_le16(rec + OFF_CRC) != crc16(rec, OFF_CRC)) {
        return 0;
    }
    atc_settings_clear(&got);
    got.config_id = read_le16(rec + OFF_SCREEN);
    got.have_mac = (uint8_t)((rec[OFF_FLAGS] & ATC_SETTINGS_HAVE_MAC) != 0u);
    memcpy(got.mac, rec + OFF_MAC, ATC_SETTINGS_MAC_BYTES);
    *out = got;
    return 1;
}

int atc_settings_bd_addr(const atc_settings *in, uint8_t addr[6])
{
    uint8_t i;
    uint8_t ones = 0;
    uint8_t zeros = 0;

    if (in == 0 || addr == 0 || !in->have_mac) {
        return 0;
    }
    /* The low six of the eight. Byte 2 of the stored address is the octet a
     * scanner prints first, and bd_addr runs the other way round.
     */
    for (i = 0; i < 6u; i++) {
        uint8_t b = in->mac[(ATC_SETTINGS_MAC_BYTES - 1u) - i];

        if (b == 0xFFu) {
            ones++;
        }
        if (b == 0x00u) {
            zeros++;
        }
        addr[i] = b;
    }
    if (ones == 6u || zeros == 6u) {
        memset(addr, 0, 6u);
        return 0;
    }
    return 1;
}
