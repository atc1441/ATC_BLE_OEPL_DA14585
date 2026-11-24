#ifndef ATC_GUI_H_
#define ATC_GUI_H_

#include <stdint.h>

/* One status line. weight is its share of the screen height. */
typedef struct {
    const char *text;
    uint8_t weight;
} gui_line;

/* Lay the lines out for the 880 by 528 glass. The strings must stay valid
 * until the refresh that paints them has finished.
 */
void gui_set_lines(const gui_line *lines, uint8_t count);

/* Paint one OEPL row. Row 0 is the far edge. Bit 7 of the first byte is the
 * left pixel, and a set bit is black ink, same as a stored 1bpp image.
 */
void gui_paint_row(uint16_t oepl_row, uint8_t *dst, uint16_t nbytes);

#endif
