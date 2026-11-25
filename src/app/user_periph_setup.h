#ifndef USER_PERIPH_SETUP_H_
#define USER_PERIPH_SETUP_H_

#include "gpio.h"

/* Boot SPI flash. These pads are the ROM boot pins and stay off the panel bus. */
#define FLASH_CLK_PORT   GPIO_PORT_0
#define FLASH_CLK_PIN    GPIO_PIN_0
#define FLASH_CS_PORT    GPIO_PORT_0
#define FLASH_CS_PIN     GPIO_PIN_3
#define FLASH_MISO_PORT  GPIO_PORT_0
#define FLASH_MISO_PIN   GPIO_PIN_5
#define FLASH_MOSI_PORT  GPIO_PORT_0
#define FLASH_MOSI_PIN   GPIO_PIN_6

/* ZKC75S 7.5" panel, taken from the tag's pin record. */
#define EPD_SCLK_PORT    GPIO_PORT_0
#define EPD_SCLK_PIN     GPIO_PIN_1
#define EPD_DC_PORT      GPIO_PORT_0
#define EPD_DC_PIN       GPIO_PIN_7
#define EPD_RST_PORT     GPIO_PORT_1
#define EPD_RST_PIN      GPIO_PIN_0
#define EPD_BUSY_PORT    GPIO_PORT_1
#define EPD_BUSY_PIN     GPIO_PIN_1
#define EPD_MOSI_PORT    GPIO_PORT_2
#define EPD_MOSI_PIN     GPIO_PIN_0
#define EPD_CS_PORT      GPIO_PORT_2
#define EPD_CS_PIN       GPIO_PIN_1
#define EPD_BS_PORT      GPIO_PORT_2
#define EPD_BS_PIN       GPIO_PIN_2
#define EPD_PWR_PORT     GPIO_PORT_2
#define EPD_PWR_PIN      GPIO_PIN_3

/* Common-anode lamps: a low pad lights them. Reset leaves every pad pulled
 * down, which is why a lamp stays on until firmware drives it high.
 */
#define LED_R_PORT       GPIO_PORT_0
#define LED_R_PIN        GPIO_PIN_2
#define LED_G_PORT       GPIO_PORT_2
#define LED_G_PIN        GPIO_PIN_5
#define LED_B_PORT       GPIO_PORT_2
#define LED_B_PIN        GPIO_PIN_9
#define LED_Y_PORT       GPIO_PORT_1
#define LED_Y_PIN        GPIO_PIN_3
/* Further unused pads. Held high so a single active-low lamp on any of them
 * is dark. P1_4 and P1_5 stay untouched: those are the SWD pins.
 */
#define LED_A_PORT       GPIO_PORT_0
#define LED_A_PIN        GPIO_PIN_4
#define LED_C_PORT       GPIO_PORT_2
#define LED_C_PIN        GPIO_PIN_4
#define LED_Z_PORT       GPIO_PORT_3
#define LED_Z_PIN        GPIO_PIN_0

/* The NFC chip's bus. These four pads used to be in the lamp set, driven
 * high so an unknown active-low lamp would be dark - which meant the
 * firmware drove the whole bus push-pull and the chip never got a word in.
 *
 * SDA and SCL carry external pull-ups, so they idle as inputs and are only
 * ever pulled down. CS has none and must be driven: high is the idle
 * state, because the chip belongs to the reader whenever the MCU is not
 * using it. IRQ is the chip's own output and is never driven.
 */
#define NFC_SDA_PORT     GPIO_PORT_2
#define NFC_SDA_PIN      GPIO_PIN_7
#define NFC_SCL_PORT     GPIO_PORT_2
#define NFC_SCL_PIN      GPIO_PIN_8
#define NFC_CS_PORT      GPIO_PORT_2
#define NFC_CS_PIN       GPIO_PIN_6
#define NFC_IRQ_PORT     GPIO_PORT_1
#define NFC_IRQ_PIN      GPIO_PIN_2

void GPIO_reservations(void);
void set_pad_functions(void);
void periph_init(void);

/* Tell the pad table that the application has changed a pad's level.
 *
 * periph_init() replays the recorded pad state on every wake from a sleep
 * long enough to drop the peripheral power domain. Anything that drives a pad
 * outside set_pad_functions() has to say so here, or the replay will put that
 * pad back to its boot level - for the lamp that means switching itself off.
 */
void atc_pad_level(unsigned port, unsigned pin, int high);
/* Same, for a pad's direction. INPUT drops it from the driven masks. */
void atc_pad_mode(unsigned port, unsigned pin, unsigned mode, unsigned func);

#endif
