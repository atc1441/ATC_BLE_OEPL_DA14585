#ifndef USER_ATC_H_
#define USER_ATC_H_

#include <stdint.h>
#include "attm_db_128.h"

/* One service. Service and characteristic are both UUID 0x1337. */
enum {
    ATC_IDX_SVC = 0,
    ATC_IDX_OEPL_CHAR,
    ATC_IDX_OEPL_VAL,
    ATC_IDX_OEPL_NTF_CFG,
    ATC_IDX_NB,
};

/* Command byte plus a 233-byte part. */
#define ATC_OEPL_MAX_LEN 235

extern struct attm_desc_128 custs1_att_db[ATC_IDX_NB];
extern const uint8_t custs1_services[];
extern const uint8_t custs1_services_size;
extern const uint16_t custs1_att_max_nb;

/* Read the settings record and act on it, from periph_init().
 *
 * It has to be this early. The SDK copies the OTP address into dev_bdaddr and
 * then starts the link layer, which takes its public address from there; and
 * the panel geometry is read into the session when the application starts. By
 * the time user_app_on_init() runs, both choices have been made. periph_init()
 * is the last point at which a stored address or a stored glass can still be
 * the one this boot uses.
 */
void atc_early_init(void);

#endif
