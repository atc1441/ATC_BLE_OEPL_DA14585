#include "gui.h"

#include <string.h>

#include "gui_font.inc"

#define GUI_WIDTH  880u
#define GUI_HEIGHT 528u
#define GUI_MAX    8u
#define CELL_W     6u
#define CELL_H     7u

typedef struct {
    const char *text;
    uint16_t x;
    uint16_t y;
    uint8_t scale;
    uint8_t len;
} placed_line;

static placed_line placed[GUI_MAX];
static uint8_t placed_n;

static uint8_t fit_scale(uint8_t len, uint16_t slot_w, uint16_t slot_h)
{
    uint8_t scale = 1u;
    uint8_t step;

    if (len == 0u || slot_w == 0u || slot_h == 0u) {
        return 1u;
    }
    for (step = 1u; step <= 16u; step++) {
        if ((uint32_t)len * CELL_W * step > slot_w || (uint32_t)CELL_H * step > slot_h) {
            break;
        }
        scale = step;
    }
    return scale;
}

void gui_set_lines(const gui_line *lines, uint8_t count)
{
    uint16_t weights = 0u;
    uint16_t y = 0u;
    uint8_t i;

    placed_n = 0u;
    if (lines == 0 || count == 0u) {
        return;
    }
    if (count > GUI_MAX) {
        count = GUI_MAX;
    }
    for (i = 0u; i < count; i++) {
        uint8_t weight = lines[i].weight ? lines[i].weight : 1u;
        weights = (uint16_t)(weights + weight);
    }
    for (i = 0u; i < count; i++) {
        uint8_t weight = lines[i].weight ? lines[i].weight : 1u;
        uint16_t slot = (uint16_t)(((uint32_t)GUI_HEIGHT * weight) / weights);
        uint16_t pix_h;
        uint16_t pix_w;
        uint8_t len = 0u;
        uint8_t scale;
        const char *text = lines[i].text ? lines[i].text : "";

        if (i + 1u == count && y < GUI_HEIGHT) {
            slot = (uint16_t)(GUI_HEIGHT - y);
        }
        while (text[len] != 0 && len < 48u) {
            len++;
        }
        scale = fit_scale(len, GUI_WIDTH, slot);
        pix_w = (uint16_t)((uint16_t)len * CELL_W * scale);
        pix_h = (uint16_t)(CELL_H * scale);
        placed[i].text = text;
        placed[i].len = len;
        placed[i].scale = scale;
        placed[i].x = (pix_w < GUI_WIDTH) ? (uint16_t)((GUI_WIDTH - pix_w) / 2u) : 0u;
        placed[i].y = y;
        if (pix_h < slot) {
            placed[i].y = (uint16_t)(y + (slot - pix_h) / 2u);
        }
        y = (uint16_t)(y + slot);
    }
    placed_n = count;
}

static void ink_span(uint8_t *dst, uint16_t nbytes, uint16_t x, uint16_t n)
{
    uint16_t limit = (uint16_t)(nbytes * 8u);

    while (n != 0u && x < limit) {
        uint16_t bit = (uint16_t)(x & 7u);
        uint16_t room = (uint16_t)(8u - bit);
        uint8_t mask;

        if (room > n) {
            room = n;
        }
        if ((uint16_t)(x + room) > limit) {
            room = (uint16_t)(limit - x);
        }
        mask = (uint8_t)(((1u << room) - 1u) << (8u - bit - room));
        dst[x >> 3] |= mask;
        x = (uint16_t)(x + room);
        n = (uint16_t)(n - room);
    }
}

void gui_paint_row(uint16_t oepl_row, uint8_t *dst, uint16_t nbytes)
{
    uint8_t i;

    if (dst == 0 || nbytes == 0u) {
        return;
    }
    memset(dst, 0, nbytes);
    for (i = 0u; i < placed_n; i++) {
        uint8_t scale = placed[i].scale;
        uint16_t top = placed[i].y;
        uint16_t pix_h = (uint16_t)(CELL_H * scale);
        uint16_t gy;
        uint8_t c;
        uint16_t x;

        if (scale == 0u || oepl_row < top || oepl_row >= (uint16_t)(top + pix_h)) {
            continue;
        }
        gy = (uint16_t)((oepl_row - top) / scale);
        if (gy >= CELL_H) {
            continue;
        }
        x = placed[i].x;
        for (c = 0u; c < placed[i].len; c++) {
            unsigned ch = (unsigned char)placed[i].text[c];
            uint8_t bits = 0u;
            uint8_t col;

            if (ch >= 32u && ch <= 126u) {
                bits = gui_font[ch - 32u][gy];
            }
            for (col = 0u; col < 5u; col++) {
                if (bits & (uint8_t)(0x10u >> col)) {
                    ink_span(dst, nbytes, x, scale);
                }
                x = (uint16_t)(x + scale);
            }
            x = (uint16_t)(x + scale);
        }
    }
}
