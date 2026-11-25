#ifndef ATC_PANEL_H_
#define ATC_PANEL_H_

#include <stdint.h>

/* One row in panel.c is one board configuration: which glass is on it, and
 * what else the PCB carries.
 *
 * Driver 1 is the ZKC75S window: data entry 0x01, X across the width, Y from
 * (height - 1) down to 0, black RAM 0x24, red RAM 0x26. Every geometry in the
 * driver is read from the row, so a different glass on that same controller is
 * only a new row. A different controller needs its own source file, a new
 * driver id here, and one row that names that id.
 */
#define ATC_DRV_NONE   0u
#define ATC_DRV_ZKC75S 1u

/* Config id 0 is "unconfigured", and it is where a freshly flashed tag
 * starts. It names no glass, so nothing is drawn and a picture is refused
 * until a host has said which board this is - command 0x0004. The ids from
 * 1 up are the real configurations, in catalog order.
 *
 * hw_type is the second number in a row and is not the same one: it is the
 * hardware type in the advertisement, which is how a host that works by
 * profile rather than by config id recognises the glass.
 */
#define ATC_CFG_NONE 0u

/* What the board carries besides the glass. The same firmware runs on PCBs
 * that have the RGB LED fitted and on ones that have nothing there, so the
 * row says which, and the firmware does not drive what is not there.
 */
#define ATC_CAP_LED 0x01u
#define ATC_CAP_NFC 0x02u

/* The picture window is not a constant. These boards carry different flash -
 * 512 KiB on the 7.5 inch one, 2 MiB on the 13.3 inch one - and a bigger
 * glass needs a bigger window than a small chip has room for. So the window
 * is worked out at boot from the size the chip reports in its JEDEC id and
 * the size of one picture: it is the top of the flash, and the last 4 KiB
 * sector of it holds the ATCIMG marker rather than picture data.
 *
 * Everything below ATC_PANEL_LOW is spoken for - bootloader, both boot
 * slots, the product header, the pin record, the waveform, the NDEF sector
 * and the settings record - so a window never starts under it.
 */
#define ATC_PANEL_MARKER 4096u
#define ATC_PANEL_LOW    0x00060000u
#define ATC_PANEL_GRAIN  0x00010000u   /* windows are whole 64 KiB blocks */
#define ATC_PANEL_MIN    0x00020000u   /* and at least 128 KiB, as before */

/* The largest picture any board here could hold: a 2 MiB chip, everything
 * below ATC_PANEL_LOW left alone, minus the marker sector. A row whose two
 * planes are bigger than this is wrong on every board and is refused
 * outright; whether it fits the chip actually fitted is a separate question
 * that atc_panel_window() answers at boot.
 */
#define ATC_PANEL_IMAGE_MAX (0x00200000u - ATC_PANEL_LOW - ATC_PANEL_MARKER)

/* One row is at most ATC_ROW_MAX bytes wide in the driver, so 1024 pixels. */
#define ATC_PANEL_WIDTH_MAX 1024u

typedef struct atc_panel {
    uint16_t config_id;   /* 0 is unconfigured; what 0x0004 selects */
    uint16_t hw_type;     /* hardware type, what goes in the advertisement */
    uint16_t width;
    uint16_t height;
    uint8_t colors;
    uint8_t driver;
    uint8_t caps;         /* ATC_CAP_LED, ATC_CAP_NFC */
    uint8_t boost;        /* last byte of the controller's 0x0C booster soft start */
    const char *name;
} atc_panel;

/* Two 1-bit planes of this glass, which is what one stored picture costs. */
uint32_t atc_panel_image_size(const atc_panel *panel);

/* Where the picture window lands on a chip of flash_bytes, as [base, end).
 * 1 when it fits, 0 when the chip is too small for this glass - and then
 * the row must not be selected, because a picture would have nowhere to go.
 */
int atc_panel_window(const atc_panel *panel, uint32_t flash_bytes,
                     uint32_t *base, uint32_t *end);

/* The row this run is on. Unconfigured until a stored config id replaces it
 * - see atc_panel_select(). Never null.
 */
const atc_panel *atc_panel_get(void);

/* 1 once a row with a glass behind it is selected. Everything that touches
 * the e-paper asks this first.
 */
int atc_panel_configured(void);

/* 1 when the selected board carries that piece of hardware. */
int atc_panel_has(uint8_t cap);

/* Null when the id is not in the catalog. */
const atc_panel *atc_panel_find(uint16_t hw_type);
const atc_panel *atc_panel_find_config(uint16_t config_id);

/* Walk the catalog. */
unsigned atc_panel_count(void);
const atc_panel *atc_panel_at(unsigned index);

/* 1 when this build can actually drive that row: a known driver, a width the
 * driver can hold, and two planes that fit the picture window. A row that
 * fails this is refused by 0x0004 rather than selected and then drawn wrong.
 */
int atc_panel_usable(const atc_panel *panel);

/* Choose the row with that config id for this run. 0 when it is not in the
 * catalog or not usable, and the selection is left alone. Called at boot with
 * whatever the settings record holds, and by command 0x0004 after it has
 * stored a new one.
 */
int atc_panel_select(uint16_t config_id);

#endif
