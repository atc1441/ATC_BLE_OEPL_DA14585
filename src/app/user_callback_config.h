#ifndef USER_CALLBACK_CONFIG_H_
#define USER_CALLBACK_CONFIG_H_

#include "app_api.h"
#include "app_callback.h"
#include "app_prf_types.h"
#if (BLE_APP_SEC)
#include "app_bond_db.h"
#endif

void user_on_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param);
void user_on_disconnect(struct gapc_disconnect_ind const *param);
void user_catch_rest_hndl(ke_msg_id_t const msgid,
                          void const *param,
                          ke_task_id_t const dest_id,
                          ke_task_id_t const src_id);
void user_app_on_init(void);
void user_on_dev_config(void);
void user_on_db_init(void);
void user_on_adv_complete(const uint8_t status);
void user_advertise(void);
void user_on_update_params_rejected(const uint8_t status);
void user_on_update_params_complete(void);
void user_before_sleep(void);
sleep_mode_t user_validate_sleep(sleep_mode_t sleep_mode);
void user_resume_from_sleep(void);

static const struct app_callbacks user_app_callbacks = {
    .app_on_connection                  = user_on_connection,
    .app_on_disconnect                  = user_on_disconnect,
    .app_on_update_params_rejected      = user_on_update_params_rejected,
    .app_on_update_params_complete      = user_on_update_params_complete,
    .app_on_set_dev_config_complete     = user_on_dev_config,
    .app_on_adv_nonconn_complete        = NULL,
    .app_on_adv_undirect_complete       = user_on_adv_complete,
    .app_on_adv_direct_complete         = NULL,
    .app_on_db_init_complete            = user_on_db_init,
    .app_on_scanning_completed          = NULL,
    .app_on_adv_report_ind              = NULL,
    .app_on_get_dev_name                = default_app_on_get_dev_name,
    .app_on_get_dev_appearance          = default_app_on_get_dev_appearance,
    .app_on_get_dev_slv_pref_params     = default_app_on_get_dev_slv_pref_params,
    .app_on_set_dev_info                = default_app_on_set_dev_info,
    .app_on_data_length_change          = NULL,
    .app_on_update_params_request       = default_app_update_params_request,
    .app_on_generate_static_random_addr = default_app_generate_static_random_addr,
    .app_on_svc_changed_cfg_ind         = NULL,
    .app_on_get_peer_features           = NULL,
#if (BLE_APP_SEC)
    .app_on_pairing_request             = default_app_on_pairing_request,
    .app_on_tk_exch                     = default_app_on_tk_exch,
    .app_on_irk_exch                    = NULL,
    .app_on_csrk_exch                   = NULL,
    .app_on_ltk_exch                    = default_app_on_ltk_exch,
    .app_on_pairing_succeeded           = NULL,
    .app_on_encrypt_ind                 = NULL,
    .app_on_encrypt_req_ind             = NULL,
    .app_on_security_req_ind            = NULL,
    .app_on_addr_solved_ind             = NULL,
    .app_on_addr_resolve_failed         = NULL,
#if !defined (__DA14531_01__) && !defined (__DA14535__)
    .app_on_ral_cmp_evt                 = NULL,
    .app_on_ral_size_ind                = NULL,
    .app_on_ral_addr_ind                = NULL,
#endif
#endif
};

#if (BLE_APP_SEC)
static const struct app_bond_db_callbacks user_app_bond_db_callbacks = {
    .app_bdb_init                      = NULL,
    .app_bdb_get_size                  = NULL,
    .app_bdb_add_entry                 = NULL,
    .app_bdb_remove_entry              = NULL,
    .app_bdb_search_entry              = NULL,
    .app_bdb_get_number_of_stored_irks = NULL,
    .app_bdb_get_stored_irks           = NULL,
    .app_bdb_get_device_info_from_slot = NULL,
};
#endif

#define app_process_catch_rest_cb user_catch_rest_hndl

static const struct default_app_operations user_default_app_operations = {
    .default_operation_adv = user_advertise,
};

static const struct arch_main_loop_callbacks user_app_main_loop_callbacks = {
    .app_on_init            = user_app_on_init,
    .app_on_ble_powered     = NULL,
    .app_on_system_powered  = NULL,
    .app_before_sleep       = user_before_sleep,
    .app_validate_sleep     = user_validate_sleep,
    .app_going_to_sleep     = NULL,
    .app_resume_from_sleep  = user_resume_from_sleep,
};

static const struct prf_func_callbacks user_prf_funcs[] = {
    {TASK_ID_INVALID, NULL, NULL},
};

#endif
