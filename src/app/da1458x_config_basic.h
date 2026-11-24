#ifndef DA1458X_CONFIG_BASIC_H_
#define DA1458X_CONFIG_BASIC_H_

#include "da1458x_stack_config.h"
#include "user_profiles_config.h"

#define CFG_APP
#define CFG_APP_SECURITY
#define CFG_WDOG
#undef CFG_WDG_TRIGGER_HW_RESET_IN_PRODUCTION_MODE
#define CFG_MAX_CONNECTIONS (1)
/* A kernel alloc failure calls platform_reset. With development debug that
 * assert freezes the watchdog, so the tag never advertises again. Leave it
 * off: the reset actually reboots and check-in comes back.
 */
#undef CFG_DEVELOPMENT_DEBUG
#undef CFG_PRINTF
#undef CFG_UART1_SDK
#undef CFG_SPI_FLASH_ENABLE
#undef CFG_I2C_EEPROM_ENABLE
#undef CFG_UART_DMA_SUPPORT
#undef CFG_SPI_DMA_SUPPORT
#undef CFG_I2C_DMA_SUPPORT

#endif
