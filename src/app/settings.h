#ifndef ATC_SETTINGS_H_
#define ATC_SETTINGS_H_

#include <stdint.h>

/* What survives a reboot.
 *
 * Two commands change a tag for good rather than for the length of a link:
 * 0x0004 picks the glass, 0x0009 sets the address. Both have to be here on
 * the next boot or they did nothing, and both are read before the radio
 * starts, so this record cannot live anywhere that needs the stack up.
 *
 * A PIN is deliberately NOT here. It is held in RAM only, so a reboot or a
 * battery change clears it, and a tag can never be locked out for good by a
 * PIN nobody remembers. That is the same rule the reference firmware states.
 *
 * The record is 32 bytes in its own 4 KiB sector. Its own sector because the
 * smallest thing this flash can erase is 4 KiB, and sharing one with the
 * NDEF message would mean rewriting a message to change an address.
 */
#define ATC_SETTINGS_BYTES   32u
#define ATC_SETTINGS_MAGIC   0x53435441u /* 'A' 'T' 'C' 'S' little-endian */
#define ATC_SETTINGS_VERSION 1u

#define ATC_SETTINGS_MAC_BYTES 8u

/* Bit 0 of the flags byte: the address field holds an address. Without it the
 * eight bytes are ignored and the chip keeps the one it booted with.
 */
#define ATC_SETTINGS_HAVE_MAC 0x01u

typedef struct atc_settings {
    uint16_t config_id;                       /* 0: unconfigured */
    uint8_t mac[ATC_SETTINGS_MAC_BYTES];      /* OEPL order, 0x1337 first */
    uint8_t have_mac;
} atc_settings;

/* The all-defaults state: unconfigured, no stored address. */
void atc_settings_clear(atc_settings *out);

/* 1 when the 32 bytes are a record this build understands and the check word
 * agrees. An erased sector (all 0xFF) and a blank one (all 0x00) both fail,
 * which is what makes "nothing stored yet" indistinguishable from "never
 * written" - and both want the same answer, the defaults.
 */
int atc_settings_decode(const uint8_t rec[ATC_SETTINGS_BYTES], atc_settings *out);

/* Lay the record out, check word included. */
void atc_settings_encode(const atc_settings *in, uint8_t rec[ATC_SETTINGS_BYTES]);

/* The six bytes the radio wants, in bd_addr order (byte 0 is the octet a
 * scanner prints last), taken from the low six of the eight stored. Returns 0
 * and writes nothing when no address is stored, or when the one stored is all
 * zeros or all ones - neither is a usable public address, and a tag that took
 * one would come up unreachable.
 */
int atc_settings_bd_addr(const atc_settings *in, uint8_t addr[6]);

#endif
