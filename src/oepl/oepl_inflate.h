#ifndef OEPL_INFLATE_H
#define OEPL_INFLATE_H

#include <stdint.h>

/* 0 keeps going. Negative aborts the stream. Positive accepts these
 * bytes and pauses so the caller can return to the kernel.
 */
typedef int (*oepl_inflate_out)(void *user, const uint8_t *data, uint16_t len);

/* One inflater. A new zlib member starts with oepl_inflate_reset. */
void oepl_inflate_reset(void);

/*
 * Feed the next bytes of one zlib member (CMF/FLG, deflate, Adler32).
 * Window sizes above 4096 are rejected. *used is how many bytes were taken.
 * Returns 1 when the member is complete and the Adler32 matches,
 * 0 when every supplied byte was consumed and more input is required,
 * 2 when the output callback paused after accepting the bytes so far,
 * -1 when the stream is not a valid zlib member.
 */
int oepl_inflate_feed(const uint8_t *data, uint16_t len, uint16_t *used,
                      oepl_inflate_out out, void *user);

#endif
