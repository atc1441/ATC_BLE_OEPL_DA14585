#ifndef ATC_RTT_H_
#define ATC_RTT_H_

#include <stdint.h>

void rtt_init(void);
void rtt_puts(const char *text);
void rtt_hex16(uint16_t value);
void rtt_u32(uint32_t value);

/* The log ring.
 *
 * Everything this module is asked to print goes to the SEGGER buffer for a
 * probe, and - while the ring is armed - into a second buffer the radio can
 * hand out as command 0x0022's stream. One sink, two readers: a tag with no
 * probe on it can still be followed over BLE, which is the whole point.
 *
 * The ring is armed only while a client is listening. Nothing is kept
 * otherwise, so an unattended tag spends no cycles and no bytes on a log
 * nobody will read.
 */
void rtt_log_enable(int on);
int rtt_log_enabled(void);

/* Bytes waiting to be streamed. */
uint16_t rtt_log_pending(void);

/* Move up to cap bytes out of the ring. What comes out is gone from the ring,
 * so a chunk that has been handed over is never handed over twice.
 */
uint16_t rtt_log_take(uint8_t *dst, uint16_t cap);

/* How many bytes the ring has thrown away because it filled up, and clear
 * that count. The oldest bytes are the ones dropped: when a tag is busy the
 * interesting line is the last one, not the first.
 */
uint16_t rtt_log_dropped(void);

#endif
