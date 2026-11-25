#include "panel.h"

/* The catalog: one row per board configuration.
 *
 * Row 0 is "unconfigured" and is where a freshly flashed tag starts. It
 * names no glass and no driver, so nothing is drawn on the e-paper and a
 * picture transfer is refused until a host has set the config with command
 * 0x0004. That is deliberate: the same image runs on boards with different
 * glass behind the same connector, and drawing 880 by 528 into a panel that
 * is not 880 by 528 is worse than drawing nothing.
 *
 * The rows from 1 up are the real configurations. Append one to add a board.
 * Keep the driver id unless the controller sequence is different.
 *
 * Row 0 advertises hardware type 0xFA, which is what every ATC_BLE_OEPL
 * firmware reports while it has no display configured: an access point
 * matches that against its ConfigMode tag type and offers the list of
 * displays instead of trying to send a picture. Setting one is an
 * AvailDataInfo of type 0xAF with argument 0xA0 + config id, or command
 * 0x0004 with the id - both end in the same place.
 *
 * caps says what else is on the PCB. Some of these boards have the RGB LED
 * fitted and an NFC chip next to it, some have neither, and the difference
 * is not visible to the firmware at runtime - so it is written down here.
 */

static const atc_panel catalog[] = {
    /* id   hw       w     h    col driver          caps                       boost  name */
    {   0u, 0x00FAu,   0u,   0u, 0u, ATC_DRV_NONE,   0u,                        0x00u, "unconfigured"       },
    {   1u, 0x0073u, 880u, 528u, 2u, ATC_DRV_ZKC75S, ATC_CAP_LED,               0x40u, "7.5 BWR ZKC75S"     },
    {   2u, 0x0074u, 960u, 680u, 2u, ATC_DRV_ZKC75S, ATC_CAP_NFC,               0x80u, "13.3 BWR ZKC133V-N" },
};

/* Row 2, the 13.3 inch board, read off its own stock firmware:
 *
 *   - 960 by 680, two 1-bit planes, 163200 bytes a picture. The board calls
 *     itself ZKC133V-N in the record at 0x39014 in its flash, with type
 *     0x0120 in front of the name; the 7.5 inch board has 0x0119 and ZKC75S
 *     there. The hardware type here is 0x74, the next free number after the
 *     0x73 the 7.5 inch glass uses, and resources/tagtypes/74.json in the
 *     access point describes it: 960 x 680, two planes, buffer unrotated.
 *   - The controller sequence is the one this driver already sends. Taken
 *     command by command out of the stock init at 0x07FC52DC, the only
 *     difference to the 7.5 inch panel is the last byte of the 0x0C booster
 *     soft start: 0x80 rather than 0x40. Hence the boost field.
 *   - The eight panel pads are the same eight, in the same places. The stock
 *     firmware keeps them in a table in RAM and it reads CS P2_1, BS P2_2,
 *     RST P1_0, SCLK P0_1, MOSI P2_0, DC P0_7, BUSY P1_1, power P2_3.
 *   - Its flash is 2 MiB (JEDEC A1 28 15) rather than 512 KiB, which is why
 *     the window sits at 0x140000: clear of everything the stock layout uses,
 *     and big enough for two planes with room to spare.
 */

/* The row chosen for this run. Null means row 0, unconfigured, which is
 * resolved on every call rather than cached at init: atc_panel_get() is
 * called before anything has had the chance to select, and a cache would
 * then hold whatever the first caller saw.
 */
static const atc_panel *selected;

unsigned atc_panel_count(void)
{
    return (unsigned)(sizeof catalog / sizeof catalog[0]);
}

const atc_panel *atc_panel_at(unsigned index)
{
    if (index >= atc_panel_count()) {
        return 0;
    }
    return &catalog[index];
}

const atc_panel *atc_panel_find(uint16_t hw_type)
{
    unsigned i;

    if (hw_type == 0u) {
        return 0;
    }
    for (i = 0; i < atc_panel_count(); i++) {
        if (catalog[i].hw_type == hw_type) {
            return &catalog[i];
        }
    }
    return 0;
}

const atc_panel *atc_panel_find_config(uint16_t config_id)
{
    unsigned i;

    if (config_id == ATC_CFG_NONE) {
        return 0;
    }
    for (i = 0; i < atc_panel_count(); i++) {
        if (catalog[i].config_id == config_id) {
            return &catalog[i];
        }
    }
    return 0;
}

uint32_t atc_panel_image_size(const atc_panel *panel)
{
    if (panel == 0) {
        return 0;
    }
    /* Two planes, because a two-colour glass stores both and a one-colour
     * glass is still offered type 0x21 by a host that does not ask first.
     */
    return (((uint32_t)panel->width * (uint32_t)panel->height) >> 3) << 1;
}

int atc_panel_window(const atc_panel *panel, uint32_t flash_bytes,
                     uint32_t *base, uint32_t *end)
{
    uint32_t want;
    uint32_t span;
    uint32_t lo;

    if (base != 0) {
        *base = 0;
    }
    if (end != 0) {
        *end = 0;
    }
    if (panel == 0 || flash_bytes < ATC_PANEL_LOW + ATC_PANEL_MIN) {
        return 0;
    }
    /* One picture, the marker sector, rounded up to whole 64 KiB blocks -
     * the erase granularity that matters here - and never smaller than the
     * 128 KiB the 7.5 inch board has always used.
     */
    want = atc_panel_image_size(panel) + ATC_PANEL_MARKER;
    span = (want + ATC_PANEL_GRAIN - 1u) & ~(ATC_PANEL_GRAIN - 1u);
    if (span < ATC_PANEL_MIN) {
        span = ATC_PANEL_MIN;
    }
    if (span > flash_bytes - ATC_PANEL_LOW) {
        return 0;
    }
    /* The top of the chip, so the layout below stays where it has always
     * been whatever size the flash turns out to be.
     */
    lo = flash_bytes - span;
    if (lo < ATC_PANEL_LOW) {
        return 0;
    }
    if (base != 0) {
        *base = lo;
    }
    if (end != 0) {
        *end = flash_bytes;
    }
    return 1;
}

int atc_panel_usable(const atc_panel *panel)
{
    uint32_t pixels;
    uint32_t plane;

    if (panel == 0 || panel->driver != ATC_DRV_ZKC75S) {
        return 0;
    }
    if (panel->width < 8u || (panel->width & 7u) != 0u
        || panel->width > ATC_PANEL_WIDTH_MAX) {
        return 0;
    }
    if (panel->height == 0u || panel->colors == 0u) {
        return 0;
    }
    /* Two planes, because a two-colour glass stores both and a one-colour
     * glass is still offered type 0x21 by a host that does not ask first.
     */
    /* How big a glass this build can carry at all. The real test is whether
     * two planes fit the window on the flash actually fitted, which only
     * the running firmware knows - atc_panel_window() answers that - but a
     * row whose picture cannot fit any of these boards is wrong wherever it
     * runs.
     */
    pixels = (uint32_t)panel->width * (uint32_t)panel->height;
    plane = pixels >> 3;
    if ((plane << 1) > ATC_PANEL_IMAGE_MAX) {
        return 0;
    }
    return 1;
}

int atc_panel_select(uint16_t config_id)
{
    const atc_panel *row = atc_panel_find_config(config_id);

    if (row == 0 || !atc_panel_usable(row)) {
        return 0;
    }
    selected = row;
    return 1;
}

const atc_panel *atc_panel_get(void)
{
    if (selected != 0) {
        return selected;
    }
    return &catalog[0];
}

int atc_panel_configured(void)
{
    return atc_panel_usable(atc_panel_get());
}

int atc_panel_has(uint8_t cap)
{
    return (atc_panel_get()->caps & cap) != 0u;
}
