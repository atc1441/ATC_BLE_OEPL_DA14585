#include "rwip_config.h"

#if (BLE_CUSTOM1_SERVER)
#include "app_customs.h"
#include "user_atc.h"
#endif
#include "user_custs_config.h"

const struct cust_prf_func_callbacks cust_prf_funcs[] = {
#if (BLE_CUSTOM1_SERVER)
    {TASK_ID_CUSTS1, custs1_att_db, ATC_IDX_NB, app_custs1_create_db, NULL, NULL, NULL},
#endif
    {TASK_ID_INVALID, NULL, 0, NULL, NULL, NULL, NULL},
};
