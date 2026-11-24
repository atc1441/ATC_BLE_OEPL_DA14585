#ifndef ATC_EPD_H_
#define ATC_EPD_H_

#include <stdint.h>

/* Start a refresh of the image stored at flash_base. The work is sliced so the
 * BLE kernel keeps running; epd_step() does one slice.
 * epd_step returns 1 while the refresh is still running, 0 when BUSY rose and
 * then fell, and -1 when BUSY never rose or stayed high.
 */
void epd_begin(uint32_t flash_base, uint8_t type);
int epd_step(void);

/* Drop a refresh between slices. The next epd_begin resets the controller. */
void epd_stop(void);

/* Same refresh as epd_begin, but each black-plane row is painted by fn.
 * fn sees OEPL row 0 at the far edge. A null red plane stays blank.
 * The bytes use the image convention: a set bit is black ink.
 */
typedef void (*epd_row_fn)(uint16_t oepl_row, uint8_t *dst, uint16_t nbytes);
void epd_begin_draw(epd_row_fn black);

/* Read the panel controller's own sensor. Returns 0 and writes degrees, or
 * -1 when the panel is busy refreshing or no pad shifted out a real value.
 * Does not start a display update.
 */
int epd_read_celsius(int8_t *celsius);

#endif
