#ifndef USER_PROFILES_CONFIG_H_
#define USER_PROFILES_CONFIG_H_

/* Heap sizes seen by both the C sources and the preprocessed linker script.
 * The message heap has to hold a 235-byte write and the matching notification.
 */
#define DB_HEAP_SZ   2048
#define MSG_HEAP_SZ  3072

#define CFG_PRF_DISS
#define CFG_PRF_CUST1

#define APP_DIS_FEATURES             (DIS_MANUFACTURER_NAME_CHAR_SUP | \
                                      DIS_MODEL_NB_STR_CHAR_SUP | \
                                      DIS_SYSTEM_ID_CHAR_SUP | \
                                      DIS_SW_REV_STR_CHAR_SUP | \
                                      DIS_FIRM_REV_STR_CHAR_SUP | \
                                      DIS_PNP_ID_CHAR_SUP)

#define APP_DIS_MANUFACTURER_NAME     ("ATC")
#define APP_DIS_MANUFACTURER_NAME_LEN (3)
#define APP_DIS_MODEL_NB_STR          ("ZKC75S")
#define APP_DIS_MODEL_NB_STR_LEN      (6)
#define APP_DIS_SYSTEM_ID             ("\xA7\xC0\xE5\x7C\x00\x85\x45\x31")
#define APP_DIS_SYSTEM_ID_LEN         (8)
#define APP_DIS_SW_REV                SDK_VERSION
#define APP_DIS_FIRM_REV              SDK_VERSION
#define APP_DIS_SERIAL_NB_STR         ("ZK-7.5")
#define APP_DIS_SERIAL_NB_STR_LEN     (6)
#define APP_DIS_HARD_REV_STR          ("DA14585")
#define APP_DIS_HARD_REV_STR_LEN      (7)
#define APP_DIS_FIRM_REV_STR          SDK_VERSION
#define APP_DIS_FIRM_REV_STR_LEN      (sizeof(APP_DIS_FIRM_REV_STR) - 1)
#define APP_DIS_SW_REV_STR            SDK_VERSION
#define APP_DIS_SW_REV_STR_LEN        (sizeof(APP_DIS_SW_REV_STR) - 1)
#define APP_DIS_IEEE                  ("\x01\x02\x03\x04\x05\x06")
#define APP_DIS_IEEE_LEN              (6)
#define APP_DIS_PNP_ID                ("\x01\xC0\xA7\x75\x0C\x01\x00")
#define APP_DIS_PNP_ID_LEN            (7)

#endif
