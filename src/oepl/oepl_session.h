#ifndef OEPL_SESSION_H
#define OEPL_SESSION_H

#include <stdint.h>

#ifdef OEPL_HOST_DLL
#define OEPL_API __declspec(dllexport)
#else
#define OEPL_API
#endif

/* Wire command numbers are big-endian on the air.
 * The public uploader also sends these around a connection and a block:
 * 0x0002 before each block, 0x0003 after the picture, 0x0011/0x0020/0x0021
 * as soon as notifications are on. Their answers are below.
 */
#define OEPL_CMD_FAST       0x0002u
#define OEPL_CMD_SLOW       0x0003u
/* Pick the glass. Payload is a big-endian ATC_BLE_OEPL panel id. The geometry
 * changes, so this one is stored and then the tag reboots.
 */
#define OEPL_CMD_SET_TYPE   0x0004u
#define OEPL_CMD_GET_INFOS  0x0005u
/* Set the address. Payload is the eight OEPL bytes. Stored, then reboot: the
 * link layer reads its public address before any of this code runs.
 */
#define OEPL_CMD_SET_MAC    0x0009u
#define OEPL_CMD_DYN_READ   0x0011u
#define OEPL_CMD_DEBUG_STS  0x0016u
/* NDEF. SET payload is [src][bytes...]. GET may carry a little-endian offset. */
#define OEPL_CMD_SET_NDEF   0x0018u
#define OEPL_CMD_GET_NDEF   0x0019u
#define OEPL_CMD_CLEAR_NDEF 0x001Au
#define OEPL_CMD_NFC_OFF    0x001Cu
#define OEPL_CMD_NFC_ON     0x001Du
/* The PIN. With a payload, 0x001E sets one and 0x001F offers one to unlock.
 * Bare, 0x001E removes the PIN and 0x001F locks the tag at once.
 */
#define OEPL_CMD_SET_PIN    0x001Eu
#define OEPL_CMD_LOCK       0x001Fu
#define OEPL_CMD_PROT       0x0020u
#define OEPL_CMD_SYSINFO    0x0021u
/* The tag's own log. Payload 01 starts the push, 00 stops it. What the tag
 * would have printed on a probe arrives as OEPL_CMD_ACK_LOG.
 */
#define OEPL_CMD_LOG        0x0022u
/* This build's own extension, not part of the reference command set: pick the
 * sleep mode at runtime. 0 is ARCH_SLEEP_OFF, 1 is extended sleep.
 *
 * It is a command rather than a build option on purpose. Extended sleep is
 * where the current actually goes - the difference is better than tenfold -
 * but it is also the one setting that can leave this chip waking into
 * something it cannot run, and a tag that does that is only recoverable with
 * a programmer. Held in RAM and defaulting to off, so the escape hatch is a
 * power cycle rather than a J-Link, and nothing can be stored that stops the
 * tag coming back.
 */
#define OEPL_CMD_SLEEP      0x0023u
#define OEPL_CMD_ACK_CMD    0x0063u
#define OEPL_CMD_AVAIL      0x0064u
#define OEPL_CMD_BLOCK_PART 0x0065u
#define OEPL_CMD_PART_BAD   0x00C4u
#define OEPL_CMD_PART_OK    0x00C5u
#define OEPL_CMD_BLOCK_REQ  0x00C6u
#define OEPL_CMD_SHOW       0x00C7u
#define OEPL_CMD_ALREADY    0x00C8u
/* Firmware finished. The page treats this as success and does not send 0x0003. */
#define OEPL_CMD_FW_OK      0x00C9u
#define OEPL_CMD_ACK_DYN    0x00CDu
/* NDEF block larger than the store. */
#define OEPL_CMD_FILE_BIG   0x00D0u
#define OEPL_CMD_ACK_STATUS 0x00D1u
/* [src][total le16][offset le16][chunk]. The chunk is whatever this notify holds. */
#define OEPL_CMD_ACK_NDEF   0x00D2u
#define OEPL_CMD_ACK_PROT   0x00D4u
#define OEPL_CMD_ACK_SYS    0x00D5u
/* Streamed log text. The payload is the bytes themselves, no header. */
#define OEPL_CMD_ACK_LOG    0x00D6u
#define OEPL_CMD_ERR        0xFFFFu

/* The PIN, and why it is only ever in RAM.
 *
 * A stored PIN that nobody remembers is a brick. These tags have no button
 * and no console, so the only way back would be a programmer on the pads -
 * and the whole point of the lock is tags that are out of reach. So the PIN
 * lives in RAM: it protects a tag for as long as it keeps running, and a
 * power cycle is the escape hatch. That is a deliberate trade and not a gap.
 *
 * An unlock is bounded twice over. OEPL_PIN_IDLE_S is how long a quiet link
 * keeps it, so a client that walks away does not leave the tag open.
 * OEPL_PIN_CEIL_S is the ceiling whatever the traffic, so a client that
 * chatters cannot hold a tag open for ever. Both are reported by 0x0020.
 */
#define OEPL_PIN_MAX        16u
#define OEPL_PIN_IDLE_S     60u
#define OEPL_PIN_CEIL_S     600u

#define OEPL_TYPE_FW        0x03u
#define OEPL_TYPE_RAW_1BPP  0x20u
#define OEPL_TYPE_RAW_2BPP  0x21u
#define OEPL_TYPE_ZLIB      0x30u
/* Block-transfer NDEF. 0xA2 is a bare message and arrives only on SET_NDEF. */
#define OEPL_NDEF_RAW       0xA0u
#define OEPL_NDEF_URL       0xA1u
#define OEPL_NDEF_MSG       0xA2u
#define OEPL_NDEF_MAX       512u
#define OEPL_NDEF_CHUNK     160u
/* Set: a reader must not be given the command channel. The NDEF file stays. */
#define OEPL_NDEF_FLAG_NO_CMDS 0x01u

/* Secondary-bootloader header, then the bytes copied to 0x07FC0000.
 * The empty application bank is 0x1F000..0x38000. A file may begin with
 * that header, or with 8 bytes whose bytes 4 and 5 are 93 27.
 */
#define OEPL_FW_HEADER      64u
#define OEPL_FW_PREFIX      8u
#define OEPL_FW_BANK        0x00019000u
#define OEPL_FW_MIN         (OEPL_FW_HEADER + 8u)
#define OEPL_FW_MAX         (OEPL_FW_BANK + OEPL_FW_PREFIX)

#define OEPL_INFOS_BYTES    176u
#define OEPL_INFOS_MAGIC    0xABDC0E98u

#define OEPL_BLOCK_IMAGE    4096u
#define OEPL_PART_DATA      230u
#define OEPL_MASK_BYTES     6u
#define OEPL_BLOCK_HDR      4u
#define OEPL_BLOCK_BODY     (OEPL_BLOCK_HDR + OEPL_BLOCK_IMAGE)
#define OEPL_PART_BYTES     (3u + OEPL_PART_DATA)
#define OEPL_AVAIL_BYTES    17u

/* Char 00001337-0000-1000-8000-00805f9b34fb, write-without-response and notify. */

typedef struct oepl_panel {
    uint16_t width;
    uint16_t height;
    uint8_t colors;
} oepl_panel;

/* This glass: ZKC75S, 880 by 528, black / white / red. */
OEPL_API extern const oepl_panel oepl_panel_zkc75s;

typedef struct oepl_notify {
    uint16_t cmd;
    uint8_t block_id;
    uint8_t type;
    uint8_t mask[OEPL_MASK_BYTES];
    uint16_t width;
    uint16_t height;
    uint8_t colors;
    uint8_t has_block;
    uint8_t has_infos;
} oepl_notify;

/* 0: the bytes are stored. 2: nothing was committed; call again with the
 * same arguments. Any other value rejects the block and asks for it again.
 */
typedef int (*oepl_store_fn)(void *user, uint32_t offset, const uint8_t *data, uint16_t len);

typedef struct oepl_session {
    uint64_t shown_ver;
    uint64_t xfer_ver;
    uint32_t xfer_size;
    uint32_t xfer_total;
    uint32_t chunk;
    oepl_store_fn store;
    void *store_user;
    uint16_t width;
    uint16_t height;
    uint8_t colors;
    uint8_t xfer_type;
    uint8_t xfer_arg;
    uint8_t block_id;
    uint8_t active;
    uint8_t parts_this;
    uint8_t has_shown;
    uint8_t show_pending;
    /* Set when a type-0x03 transfer has been stored. The picture path stays idle. */
    uint8_t fw_pending;
    /* Panel type passed to the glass: 0x20 or 0x21. A 0x30 transfer sets this
     * from the inflated plane count.
     */
    uint8_t show_type;
    /* The access point's compiler aligns the 17-byte records to 24 bytes.
     * A 24-byte avail keeps the replies in that shape for the rest of the transfer.
     */
    uint8_t wide_req;
    uint8_t z_pref;
    uint8_t z_hdrn;
    uint8_t z_planes;
    uint8_t z_fill;
    uint8_t z_have_decl;
    uint8_t z_pref_buf[4];
    uint8_t z_hdr_buf[6];
    uint8_t z_stage[128];
    uint32_t z_decl;
    uint32_t z_plain;
    uint32_t z_store;
    /* Offset into the current block already fed to the inflater.
     * z_step_max is how many 128-byte flash writes one kernel turn may do.
     * Zero runs the whole block before returning. z_more stays set until
     * the block has been inflated.
     */
    uint16_t z_block_pos;
    uint8_t z_step_max;
    uint8_t z_budget_left;
    uint8_t z_more;
    /* The block in body[] is complete, but store() asked to be called again
     * before the next notification. A 2 from store() sets this.
     */
    uint8_t store_more;
    uint8_t mask[OEPL_MASK_BYTES];
    uint8_t body[OEPL_BLOCK_BODY];
    /* Shared by the advertising record and the connected readouts.
     * Stay zero until the firmware has a real value.
     */
    uint16_t hw_type;
    uint16_t capabilities;
    uint16_t fw_version;
    /* Advertising interval in 0.625 ms slots. 3200 slots is 2 s. */
    uint32_t adv_slots;
    uint16_t battery_mv;
    /* Sum of the two VBAT samples. 1705 is 3.000 V. Zero until measured. */
    uint16_t battery_raw;
    int8_t temperature;
    uint8_t mac[6];
    /* The eight OEPL address bytes, 0x1337 first. Reported by sysinfo. Zero
     * until the firmware fills it.
     */
    uint8_t mac8[8];
    /* The ATC_BLE_OEPL panel id of the glass being driven, for sysinfo and
     * for 0x0004 to compare against. Zero when the build has no id for it.
     */
    uint16_t screen_type;
    /* The PIN, held in RAM only - see OEPL_PIN_MAX above.
     * pin_len 0 means no PIN is set, and then locked is always 0.
     * idle counts seconds since the last command, age seconds since the
     * unlock. Both are driven by oepl_pin_tick().
     */
    uint8_t pin[OEPL_PIN_MAX];
    uint8_t pin_len;
    uint8_t locked;
    uint16_t pin_idle;
    uint16_t pin_age;
    /* 1 while a client wants the log pushed. Cleared when the link drops, so
     * a tag is never left streaming to whoever connects next.
     */
    uint8_t log_stream;
    /* A block that arrived complete and then failed its own length-and-sum
     * header. The block is asked for again, which is right, but doing it
     * without saying so made a corrupt transfer look like a silent one: the
     * host sees a fresh block request and cannot tell it apart from the
     * first. These three are what the firmware logs so it can be told.
     */
    uint16_t block_bad;
    uint16_t block_bad_size;
    uint16_t block_bad_sum;
    /* Bare NDEF message. The firmware copies this to its own flash sector.
     * ndef_dirty is 1 until that copy has been written.
     */
    uint8_t ndef_src;
    uint8_t ndef_flags;
    uint8_t ndef_dirty;
    uint16_t ndef_len;
    uint8_t ndef_msg[OEPL_NDEF_MAX];
} oepl_session;

/* Sum of bytes[1 .. len). A one-byte buffer sums to 0. */
OEPL_API uint8_t oepl_checksum_of(const uint8_t *bytes, uint16_t len);
OEPL_API int oepl_checksum_ok(const uint8_t *bytes, uint16_t len);

/* zlib CRC. A first call passes 0. Later calls pass the previous result. */
OEPL_API uint32_t oepl_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
OEPL_API uint32_t oepl_crc32(const uint8_t *data, uint32_t len);

typedef struct oepl_fw_view {
    uint32_t image_off;
    uint32_t image_len;
    uint32_t code_size;
    uint32_t crc;
} oepl_fw_view;

/* 1 when the file start is this boot image. Bytes 8..11 equal to
 * 4B 4E 4C 54 are refused. The payload CRC is copied out and is not
 * checked against the payload here.
 */
OEPL_API int oepl_fw_open(const uint8_t *head, uint32_t head_len, uint32_t file_len,
                          oepl_fw_view *out);

/* 1 when the first two words are a DA14585 RAM vector table. */
OEPL_API int oepl_fw_vectors(const uint8_t vectors[8]);

OEPL_API void oepl_frame_put(uint8_t dst[2], uint16_t cmd);
OEPL_API uint16_t oepl_frame_get(const uint8_t src[2]);

/* Advertising payload without the 3-byte flags the controller adds.
 * Manufacturer record is version 2: company 0x1337, hardware type, firmware
 * version, capabilities, battery, temperature, then a counter. The complete
 * local name follows in the same packet. Returns 0 when it does not fit.
 */
OEPL_API uint8_t oepl_adv_payload(uint8_t *dst, uint8_t cap,
                                  uint16_t hw_type, uint16_t fw_version,
                                  uint16_t capabilities, uint16_t battery_mv,
                                  int8_t temperature, uint8_t counter,
                                  const uint8_t *name, uint8_t name_len);

/* Image bytes expected for a raw type. Returns 0 when the type or geometry is unusable. */
OEPL_API int oepl_expected_size(const oepl_panel *panel, uint8_t type, uint32_t *size_out);

/* 4-byte header (length, byte-sum, both little-endian) plus the image bytes. */
OEPL_API uint16_t oepl_pack_block(uint8_t *dst, uint16_t cap,
                                  const uint8_t *image, uint16_t image_len);

OEPL_API int oepl_part_count(uint16_t body_len);

/* 233-byte part. Byte 0 is the checksum of the other 232 bytes. */
OEPL_API int oepl_fill_part(uint8_t *dst, uint8_t block_id, uint8_t part_id,
                            const uint8_t *body, uint16_t body_len);

/*
 * Copy one part into a block body. A bad checksum leaves the mask alone and
 * returns 0. A good part clears its missing bit and returns 1.
 */
OEPL_API int oepl_reassemble_part(uint8_t *body, uint16_t body_len, uint8_t *mask,
                                  const uint8_t *part, uint16_t part_len);

OEPL_API void oepl_mask_fill(uint8_t mask[OEPL_MASK_BYTES], uint8_t part_count);
OEPL_API int oepl_mask_pending(const uint8_t mask[OEPL_MASK_BYTES], uint8_t part_count);

/* Full write: command plus AvailDataInfo. Checksum is the sum of the following bytes. */
OEPL_API uint16_t oepl_build_avail_frame(uint8_t *dst, uint16_t cap,
                                         uint64_t ver, uint32_t size, uint8_t type,
                                         uint8_t arg, uint16_t next_check);

/* Full write: command plus one 233-byte part. */
OEPL_API uint16_t oepl_build_part_frame(uint8_t *dst, uint16_t cap,
                                        uint8_t block_id, uint8_t part_id,
                                        const uint8_t *body, uint16_t body_len);

/* Notification parser. Block id of a block request is message byte 11. */
OEPL_API int oepl_parse_notify(const uint8_t *msg, uint16_t len, oepl_notify *out);

OEPL_API void oepl_session_init(oepl_session *session, const oepl_panel *panel,
                                oepl_store_fn store, void *user);

/* req is one GATT write. rsp is the notification to send back. */
OEPL_API uint16_t oepl_session_handle(oepl_session *session,
                                      const uint8_t *req, uint16_t req_len,
                                      uint8_t *rsp, uint16_t rsp_cap);

/* One slice of a zlib block that oepl_session_handle paused.
 * Returns 0 while session->z_more is still set.
 */
OEPL_API uint16_t oepl_session_zlib_poll(oepl_session *session,
                                         uint8_t *rsp, uint16_t rsp_cap);

/* One more attempt at a store() that returned 2.
 * Returns 0 while session->store_more is still set.
 */
OEPL_API uint16_t oepl_session_store_poll(oepl_session *session,
                                          uint8_t *rsp, uint16_t rsp_cap);

OEPL_API void oepl_session_missing(const oepl_session *session, uint8_t mask[OEPL_MASK_BYTES]);

/* Give up on the transfer in progress and go back to idle.
 *
 * For a host that stopped talking half way through a block. Without this the
 * tag keeps a transfer open for ever, which means it also keeps refusing the
 * deep sleep, the reboot and the next AvailDataInfo - one abandoned upload
 * and the tag needs a power cycle. The stored picture is left alone; only
 * the session state is dropped.
 */
OEPL_API void oepl_session_abort(oepl_session *session);

/* The lock gate.
 *
 * 1 when a locked tag will answer this command anyway. Only two qualify:
 * reading the protection state, so a client can find out why it is being
 * refused, and offering a PIN, so it can do something about it. Everything
 * else is refused with 0xFFFF while locked.
 *
 * The caller applies this before it dispatches, including to the commands
 * the firmware handles outside oepl_session_handle - a lock that covered
 * only the protocol module would leave the lamp and the reboot open.
 */
OEPL_API int oepl_pin_allows(const oepl_session *session, uint16_t cmd);

/* Advance both unlock clocks by that many seconds and relock when either
 * runs out. Safe to call on a tag with no PIN; it does nothing.
 */
OEPL_API void oepl_pin_tick(oepl_session *session, uint16_t seconds);

/* A command arrived: the idle clock starts again. The ceiling does not. */
OEPL_API void oepl_pin_touch(oepl_session *session);

/* Lock now, if a PIN is set. Called by 0x001F with no payload, and when the
 * link drops - the next client has to offer the PIN of its own.
 */
OEPL_API void oepl_pin_lock(oepl_session *session);

#endif
