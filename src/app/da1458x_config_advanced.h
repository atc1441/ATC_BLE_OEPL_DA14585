#ifndef DA1458X_CONFIG_ADVANCED_H_
#define DA1458X_CONFIG_ADVANCED_H_

#include "da14585_config_advanced.h"

/* The template selects an external 32 kHz crystal. This tag's BLE core then
 * never produces events, so the main loop sleeps until the watchdog fires.
 * The internal RCX runs without that crystal. Extended sleep still dies in
 * the wake calibration, so the sleep mode in user_config.h stays off.
 */
#undef CFG_LP_CLK
#define CFG_LP_CLK LP_CLK_RCX20
#undef CFG_XTAL16M_ADAPTIVE_SETTLING

#endif
