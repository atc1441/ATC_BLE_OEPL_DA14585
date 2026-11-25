/* The NFC tag chip on the board, over I2C.
 *
 * Boards whose config carries ATC_CAP_NFC have a dual interface NFC chip
 * next to the MCU: a reader talks to it over the air, and the MCU talks to
 * it over I2C, so whatever record the access point sends can be put where
 * a phone will find it.
 *
 * The bus is bit-banged rather than driven by the DA14585's I2C block. The
 * block was tried first and would only ever return the same byte, while a
 * hand-clocked master on the same two pads read the chip correctly at the
 * first attempt, so there is no reason to fight it: two pads and a few
 * microseconds of delay are all this needs.
 */
#ifndef ATC_NFC_H
#define ATC_NFC_H

#include <stdint.h>

/* Where the type 2 tag's data area starts, as read out of this chip: the
 * UID and the capability container sit below it and are left alone.
 */
#define ATC_NFC_DATA_AT 0x0010u
#define ATC_NFC_PAGE    16u

/* Select the chip and check that it answers. Returns 1 when it does.
 * Every call must be paired with atc_nfc_end(), which hands the chip back
 * to its radio side - while CS is low the reader cannot have it.
 */
int atc_nfc_begin(void);
void atc_nfc_end(void);

/* One page at a time, so a long record does not block the kernel: the
 * caller writes a page per timer tick. len must be at most ATC_NFC_PAGE
 * and must not cross a page boundary. Returns 1 on success.
 */
int atc_nfc_page(uint16_t offset, const uint8_t *data, uint8_t len);

/* Read straight through; the chip counts the address on by itself. */
int atc_nfc_read(uint16_t offset, uint8_t *buf, uint16_t len);

#endif /* ATC_NFC_H */
