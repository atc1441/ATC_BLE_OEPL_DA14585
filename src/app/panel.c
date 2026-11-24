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
 * caps says what else is on the PCB. Some of these boards have the RGB LED
 * fitted and an NFC chip next to it, some have neither, and the difference
 * is not visible to the firmware at runtime - so it is written down here.
 */

static const atc_panel catalog[] = {
    /* id   hw       w     h    col driver          caps          name */
    {   0u, 0x0000u,   0u,   0u, 0u, ATC_DRV_NONE,  0u,           "unconfigured"   },
    {   1u, 0x0073u, 880u, 528u, 2u, ATC_DRV_ZKC75S, ATC_CAP_LED, "7.5 BWR ZKC75S" },
};

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
    pixels = (uint32_t)panel->width * (uint32_t)panel->height;
    plane = pixels >> 3;
    if ((plane << 1) > ATC_PANEL_WINDOW) {
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
