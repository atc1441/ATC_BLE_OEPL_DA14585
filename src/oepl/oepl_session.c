#include "oepl_session.h"
#include "oepl_inflate.h"

#include <string.h>

/* Colors exclude white. 2 is black plus red. 3 would enable yellow. */
const oepl_panel oepl_panel_zkc75s = { 880, 528, 2 };

uint8_t oepl_checksum_of(const uint8_t *bytes, uint16_t len)
{
    uint8_t sum = 0;
    uint16_t i;

    if (bytes == 0 || len < 2) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum;
}

int oepl_checksum_ok(const uint8_t *bytes, uint16_t len)
{
    if (bytes == 0 || len < 2) {
        return 0;
    }
    return bytes[0] == oepl_checksum_of(bytes, len);
}

uint32_t oepl_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t oepl_crc32(const uint8_t *data, uint32_t len)
{
    return oepl_crc32_update(0, data, len);
}

void oepl_frame_put(uint8_t dst[2], uint16_t cmd)
{
    dst[0] = (uint8_t)(cmd >> 8);
    dst[1] = (uint8_t)cmd;
}

uint16_t oepl_frame_get(const uint8_t src[2])
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

int oepl_expected_size(const oepl_panel *panel, uint8_t type, uint32_t *size_out)
{
    uint32_t pixels;
    uint32_t plane;

    if (panel == 0 || size_out == 0 || panel->width == 0 || panel->height == 0) {
        return 0;
    }
    if (type != OEPL_TYPE_RAW_1BPP && type != OEPL_TYPE_RAW_2BPP) {
        return 0;
    }
    pixels = (uint32_t)panel->width * (uint32_t)panel->height;
    if ((pixels & 7u) != 0) {
        return 0;
    }
    plane = pixels >> 3;
    *size_out = (type == OEPL_TYPE_RAW_2BPP) ? (plane << 1) : plane;
    return 1;
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

static void write_le64(uint8_t *dst, uint64_t value)
{
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint64_t read_le64(const uint8_t *src)
{
    uint64_t value = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        value = (value << 8) | src[i];
    }
    return value;
}

uint16_t oepl_pack_block(uint8_t *dst, uint16_t cap,
                         const uint8_t *image, uint16_t image_len)
{
    uint16_t sum = 0;
    uint16_t i;

    if (dst == 0 || (image == 0 && image_len != 0)) {
        return 0;
    }
    if ((uint32_t)image_len + OEPL_BLOCK_HDR > cap || image_len > OEPL_BLOCK_IMAGE) {
        return 0;
    }
    for (i = 0; i < image_len; i++) {
        sum = (uint16_t)(sum + image[i]);
    }
    write_le16(dst, image_len);
    write_le16(dst + 2, sum);
    if (image_len != 0) {
        memcpy(dst + OEPL_BLOCK_HDR, image, image_len);
    }
    return (uint16_t)(OEPL_BLOCK_HDR + image_len);
}

int oepl_part_count(uint16_t body_len)
{
    if (body_len == 0) {
        return 0;
    }
    return (int)((body_len + OEPL_PART_DATA - 1u) / OEPL_PART_DATA);
}

void oepl_mask_fill(uint8_t mask[OEPL_MASK_BYTES], uint8_t part_count)
{
    uint8_t i;

    memset(mask, 0, OEPL_MASK_BYTES);
    for (i = 0; i < part_count && i < (OEPL_MASK_BYTES * 8u); i++) {
        mask[i / 8u] = (uint8_t)(mask[i / 8u] | (uint8_t)(1u << (i % 8u)));
    }
}

int oepl_mask_pending(const uint8_t mask[OEPL_MASK_BYTES], uint8_t part_count)
{
    uint8_t i;

    for (i = 0; i < part_count; i++) {
        if (mask[i / 8u] & (uint8_t)(1u << (i % 8u))) {
            return 1;
        }
    }
    return 0;
}

int oepl_fill_part(uint8_t *dst, uint8_t block_id, uint8_t part_id,
                   const uint8_t *body, uint16_t body_len)
{
    uint16_t offset;
    uint16_t count;

    if (dst == 0 || body == 0) {
        return -1;
    }
    offset = (uint16_t)part_id * OEPL_PART_DATA;
    if (offset >= body_len) {
        return -1;
    }
    count = (uint16_t)(body_len - offset);
    if (count > OEPL_PART_DATA) {
        count = OEPL_PART_DATA;
    }
    memset(dst, 0, OEPL_PART_BYTES);
    dst[1] = block_id;
    dst[2] = part_id;
    memcpy(dst + 3, body + offset, count);
    dst[0] = oepl_checksum_of(dst, OEPL_PART_BYTES);
    return 0;
}

int oepl_reassemble_part(uint8_t *body, uint16_t body_len, uint8_t *mask,
                         const uint8_t *part, uint16_t part_len)
{
    uint8_t padded[OEPL_PART_BYTES];
    uint16_t offset;
    uint16_t count;
    uint8_t part_id;

    if (body == 0 || mask == 0 || part == 0 || part_len < 3u) {
        return 0;
    }
    if (part_len > OEPL_PART_BYTES) {
        part_len = OEPL_PART_BYTES;
    }
    memset(padded, 0, sizeof padded);
    memcpy(padded, part, part_len);
    if (!oepl_checksum_ok(padded, OEPL_PART_BYTES)) {
        return 0;
    }
    part_id = padded[2];
    if (part_id >= (OEPL_MASK_BYTES * 8u)) {
        return 0;
    }
    offset = (uint16_t)part_id * OEPL_PART_DATA;
    if (offset >= body_len) {
        return 0;
    }
    count = (uint16_t)(body_len - offset);
    if (count > OEPL_PART_DATA) {
        count = OEPL_PART_DATA;
    }
    memcpy(body + offset, padded + 3, count);
    mask[part_id / 8u] = (uint8_t)(mask[part_id / 8u] & (uint8_t)~(1u << (part_id % 8u)));
    return 1;
}

uint16_t oepl_build_avail_frame(uint8_t *dst, uint16_t cap,
                                uint64_t ver, uint32_t size, uint8_t type,
                                uint8_t arg, uint16_t next_check)
{
    uint8_t *info;

    if (dst == 0 || cap < (2u + OEPL_AVAIL_BYTES)) {
        return 0;
    }
    oepl_frame_put(dst, OEPL_CMD_AVAIL);
    info = dst + 2;
    memset(info, 0, OEPL_AVAIL_BYTES);
    write_le64(info + 1, ver);
    write_le32(info + 9, size);
    info[13] = type;
    info[14] = arg;
    write_le16(info + 15, next_check);
    info[0] = oepl_checksum_of(info, OEPL_AVAIL_BYTES);
    return (uint16_t)(2u + OEPL_AVAIL_BYTES);
}

uint16_t oepl_build_part_frame(uint8_t *dst, uint16_t cap,
                               uint8_t block_id, uint8_t part_id,
                               const uint8_t *body, uint16_t body_len)
{
    if (dst == 0 || cap < (2u + OEPL_PART_BYTES)) {
        return 0;
    }
    oepl_frame_put(dst, OEPL_CMD_BLOCK_PART);
    if (oepl_fill_part(dst + 2, block_id, part_id, body, body_len) != 0) {
        return 0;
    }
    return (uint16_t)(2u + OEPL_PART_BYTES);
}

int oepl_parse_notify(const uint8_t *msg, uint16_t len, oepl_notify *out)
{
    if (msg == 0 || out == 0 || len < 2u) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->cmd = oepl_frame_get(msg);
    if (out->cmd == OEPL_CMD_BLOCK_REQ && len >= 19u) {
        out->has_block = 1;
        out->block_id = msg[11];
        out->type = msg[12];
        memcpy(out->mask, msg + 13, OEPL_MASK_BYTES);
    } else if (out->cmd == OEPL_CMD_GET_INFOS && len >= (2u + OEPL_INFOS_BYTES)) {
        out->has_infos = 1;
        out->height = read_le16(msg + 2u + 22u);
        out->width = read_le16(msg + 2u + 24u);
        out->colors = msg[2u + 30u];
    }
    return 1;
}

static uint16_t reply_cmd(uint8_t *rsp, uint16_t cap, uint16_t cmd)
{
    if (rsp == 0 || cap < 2u) {
        return 0;
    }
    oepl_frame_put(rsp, cmd);
    return 2;
}

static uint16_t reply_infos(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    uint8_t *rec;
    static const char name[] = "ATC_OEPL";

    if (rsp == 0 || cap < (2u + OEPL_INFOS_BYTES)) {
        return 0;
    }
    memset(rsp, 0, 2u + OEPL_INFOS_BYTES);
    oepl_frame_put(rsp, OEPL_CMD_GET_INFOS);
    rec = rsp + 2;
    write_le32(rec + 0, OEPL_INFOS_MAGIC);
    write_le32(rec + 4, 1u);
    write_le32(rec + 8, OEPL_INFOS_BYTES);
    write_le16(rec + 22, session->height);
    write_le16(rec + 24, session->width);
    rec[30] = session->colors;
    rec[39] = 1u;
    memcpy(rec + 61, name, 8);
    rec[101] = 8;
    return (uint16_t)(2u + OEPL_INFOS_BYTES);
}

static uint16_t reply_block(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    uint8_t *req;

    if (rsp == 0) {
        return 0;
    }
    if (session->wide_req) {
        /* checksum, 7 pad bytes, version, block, type, 6-byte mask. */
        if (cap < 26u) {
            return 0;
        }
        oepl_frame_put(rsp, OEPL_CMD_BLOCK_REQ);
        req = rsp + 2;
        memset(req, 0, 24);
        write_le64(req + 8, session->xfer_ver);
        req[16] = session->block_id;
        req[17] = session->xfer_type;
        memcpy(req + 18, session->mask, OEPL_MASK_BYTES);
        req[0] = oepl_checksum_of(req, 24);
        return 26;
    }
    if (cap < 19u) {
        return 0;
    }
    oepl_frame_put(rsp, OEPL_CMD_BLOCK_REQ);
    req = rsp + 2;
    memset(req, 0, OEPL_AVAIL_BYTES);
    write_le64(req + 1, session->xfer_ver);
    req[9] = session->block_id;
    req[10] = session->xfer_type;
    memcpy(req + 11, session->mask, OEPL_MASK_BYTES);
    req[0] = oepl_checksum_of(req, OEPL_AVAIL_BYTES);
    return 19;
}

static void begin_block(oepl_session *session)
{
    uint32_t chunk = session->xfer_size;
    uint16_t body_len;

    if (chunk > OEPL_BLOCK_IMAGE) {
        chunk = OEPL_BLOCK_IMAGE;
    }
    session->chunk = chunk;
    body_len = (uint16_t)(OEPL_BLOCK_HDR + chunk);
    session->parts_this = (uint8_t)oepl_part_count(body_len);
    oepl_mask_fill(session->mask, session->parts_this);
    memset(session->body, 0, sizeof session->body);
}

static int avail_checksum_ok(const uint8_t *info)
{
    /* Hosts that predate a checked field send 0xFF and still mean this image. */
    return oepl_checksum_ok(info, OEPL_AVAIL_BYTES) || info[0] == 0xFFu;
}

static int block_bytes_ok(const oepl_session *session)
{
    uint16_t size;
    uint16_t sum;
    uint16_t got;
    uint16_t i;

    size = read_le16(session->body);
    sum = read_le16(session->body + 2);
    if (size != session->chunk) {
        return 0;
    }
    got = 0;
    for (i = 0; i < size; i++) {
        got = (uint16_t)(got + session->body[OEPL_BLOCK_HDR + i]);
    }
    return got == sum;
}

static void z_reset(oepl_session *session)
{
    session->z_decl = 0;
    session->z_plain = 0;
    session->z_store = 0;
    session->z_pref = 0;
    session->z_hdrn = 0;
    session->z_planes = 0;
    session->z_fill = 0;
    session->z_have_decl = 0;
    session->z_block_pos = 0;
    session->z_budget_left = 0;
    session->z_more = 0;
    memset(session->z_pref_buf, 0, sizeof session->z_pref_buf);
    memset(session->z_hdr_buf, 0, sizeof session->z_hdr_buf);
    memset(session->z_stage, 0, sizeof session->z_stage);
}

static uint32_t z_plane_bytes(const oepl_session *session)
{
    return ((uint32_t)session->width * (uint32_t)session->height) >> 3;
}

/* The inflated stream is always the 6-byte header plus the planes.
 * One container declares that whole length. The web uploader declares
 * only the pixel count and still sends the header inside the zlib member.
 */
static uint32_t z_stream_limit(const oepl_session *session)
{
    uint32_t plane = z_plane_bytes(session);
    uint32_t decl = session->z_decl;

    if (decl == plane || decl == (plane << 1)) {
        return decl + 6u;
    }
    return decl;
}

static int z_decl_ok(const oepl_session *session, uint32_t decl)
{
    uint32_t pixels;
    uint32_t plane;

    pixels = (uint32_t)session->width * (uint32_t)session->height;
    if (session->width == 0 || (pixels % 8u) != 0u) {
        return 0;
    }
    plane = pixels >> 3;
    return decl == plane || decl == (plane << 1)
        || decl == (6u + plane) || decl == (6u + (plane << 1));
}

static int zlib_size_ok(const oepl_session *session, uint32_t size)
{
    uint32_t pixels;
    uint32_t plane;
    uint32_t worst;

    pixels = (uint32_t)session->width * (uint32_t)session->height;
    if (session->width == 0 || session->height == 0 || (pixels % 8u) != 0u) {
        return 0;
    }
    plane = pixels >> 3;
    /* Length prefix plus a stored-block zlib of the 6-byte header and both planes. */
    worst = 4u + 6u + (plane << 1) + 256u;
    return size >= 8u && size <= worst;
}

static int z_flush(oepl_session *session)
{
    if (session->z_fill == 0) {
        return 0;
    }
    if (session->store != 0) {
        if (session->store(session->store_user, session->z_store,
                           session->z_stage, session->z_fill) != 0) {
            return -1;
        }
    }
    session->z_store += session->z_fill;
    session->z_fill = 0;
    return 0;
}

static int z_header_ok(oepl_session *session)
{
    uint16_t width;
    uint16_t height;
    uint8_t planes;
    uint32_t plane;
    uint32_t expect;

    width = read_le16(session->z_hdr_buf + 1);
    height = read_le16(session->z_hdr_buf + 3);
    planes = session->z_hdr_buf[5];
    if (session->z_hdr_buf[0] != 6u || width != session->width || height != session->height) {
        return 0;
    }
    if (planes != 1u && planes != 2u) {
        return 0;
    }
    plane = ((uint32_t)session->width * (uint32_t)session->height) >> 3;
    expect = plane * (uint32_t)planes;
    if (session->z_decl != expect && session->z_decl != (6u + expect)) {
        return 0;
    }
    session->z_planes = planes;
    session->show_type = (planes == 2u) ? (uint8_t)OEPL_TYPE_RAW_2BPP
                                        : (uint8_t)OEPL_TYPE_RAW_1BPP;
    return 1;
}

static int z_out(void *user, const uint8_t *data, uint16_t len)
{
    oepl_session *session = (oepl_session *)user;
    uint16_t i;
    int pause = 0;

    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];

        if (session->z_plain >= z_stream_limit(session)) {
            return -1;
        }
        session->z_plain++;
        if (session->z_hdrn < 6u) {
            session->z_hdr_buf[session->z_hdrn++] = byte;
            if (session->z_hdrn == 6u && !z_header_ok(session)) {
                return -1;
            }
            continue;
        }
        session->z_stage[session->z_fill++] = byte;
        if (session->z_fill == (uint8_t)sizeof session->z_stage) {
            if (z_flush(session) != 0) {
                return -1;
            }
            /* A full plane is about a hundred of these writes. Doing them
             * inside the GATT callback fills the kernel heap and the stack
             * calls platform_reset. A step limit returns between writes.
             */
            if (session->z_step_max != 0u) {
                if (session->z_budget_left > 0u) {
                    session->z_budget_left--;
                }
                if (session->z_budget_left == 0u) {
                    pause = 1;
                }
            }
        }
    }
    return pause ? 1 : 0;
}

/* 0: need more container bytes. 1: planes stored. -1: reject the transfer. */
static int z_feed_bytes(oepl_session *session, const uint8_t *data, uint16_t len,
                        uint16_t *consumed)
{
    uint16_t pos = 0;

    if (session->z_step_max != 0u) {
        session->z_budget_left = session->z_step_max;
    }
    while (pos < len && session->z_pref < 4u) {
        session->z_pref_buf[session->z_pref++] = data[pos++];
    }
    if (session->z_pref == 4u && !session->z_have_decl) {
        session->z_decl = read_le32(session->z_pref_buf);
        session->z_have_decl = 1;
        if (!z_decl_ok(session, session->z_decl)) {
            return -1;
        }
    }
    while (pos < len) {
        uint16_t used = 0;
        uint16_t chunk = (uint16_t)(len - pos);
        int rc = oepl_inflate_feed(data + pos, chunk, &used, z_out, session);

        if (used > chunk) {
            return -1;
        }
        pos = (uint16_t)(pos + used);
        if (rc == 2) {
            if (consumed != 0) {
                *consumed = pos;
            }
            return 2;
        }
        if (rc < 0) {
            return -1;
        }
        if (rc == 1) {
            if (pos != len || z_flush(session) != 0) {
                return -1;
            }
            if (!session->z_have_decl || session->z_plain != z_stream_limit(session)
                || session->z_hdrn != 6u) {
                return -1;
            }
            if (consumed != 0) {
                *consumed = pos;
            }
            return 1;
        }
        if (used == 0) {
            return -1;
        }
    }
    /* A pause can consume the whole block while the inflater still has
     * output or the Adler32 left in its bit buffer. Pumping with an empty
     * slice finishes that without asking for a block that does not exist.
     */
    if (pos == len && session->z_have_decl && session->z_hdrn != 0u) {
        uint16_t used = 0;
        int rc = oepl_inflate_feed(data + pos, 0, &used, z_out, session);

        if (rc == 2) {
            if (consumed != 0) {
                *consumed = pos;
            }
            return 2;
        }
        if (rc < 0) {
            return -1;
        }
        if (rc == 1) {
            if (z_flush(session) != 0) {
                return -1;
            }
            if (session->z_plain != z_stream_limit(session) || session->z_hdrn != 6u) {
                return -1;
            }
            if (consumed != 0) {
                *consumed = pos;
            }
            return 1;
        }
    }
    if (consumed != 0) {
        *consumed = pos;
    }
    return 0;
}

static uint16_t fail_transfer(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    session->active = 0;
    session->z_more = 0;
    session->z_block_pos = 0;
    session->store_more = 0;
    return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
}

static uint16_t mark_shown(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap);

static uint16_t zlib_finish_block(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    const uint8_t *data;
    uint16_t len;
    uint16_t got = 0;
    int zrc;

    if (session->z_block_pos > session->chunk) {
        return fail_transfer(session, rsp, rsp_cap);
    }
    data = session->body + OEPL_BLOCK_HDR + session->z_block_pos;
    len = (uint16_t)(session->chunk - session->z_block_pos);
    zrc = z_feed_bytes(session, data, len, &got);
    session->z_block_pos = (uint16_t)(session->z_block_pos + got);
    if (zrc == 2) {
        session->z_more = 1;
        return 0;
    }
    session->z_more = 0;
    session->z_block_pos = 0;
    if (zrc < 0) {
        return fail_transfer(session, rsp, rsp_cap);
    }
    session->xfer_size -= session->chunk;
    if (zrc == 1) {
        if (session->xfer_size != 0) {
            return fail_transfer(session, rsp, rsp_cap);
        }
        return mark_shown(session, rsp, rsp_cap);
    }
    if (session->xfer_size == 0) {
        return fail_transfer(session, rsp, rsp_cap);
    }
    session->block_id = (uint8_t)(session->block_id + 1u);
    begin_block(session);
    return reply_block(session, rsp, rsp_cap);
}

static uint16_t mark_shown(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    session->active = 0;
    session->has_shown = 1;
    session->shown_ver = session->xfer_ver;
    session->show_pending = 1;
    return reply_cmd(rsp, rsp_cap, OEPL_CMD_SHOW);
}

static uint16_t mark_firmware(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    session->active = 0;
    session->fw_pending = 1;
    return reply_cmd(rsp, rsp_cap, OEPL_CMD_FW_OK);
}

static int ndef_xfer(uint8_t type)
{
    return type == OEPL_NDEF_RAW || type == OEPL_NDEF_URL;
}

/* NFC Forum URI identifier codes. The longer prefix of a scheme is first. */
static int uri_prefix(const uint8_t *buf, uint16_t len, uint8_t *code, uint16_t *skip)
{
    static const uint8_t https_www[] = "https://www.";
    static const uint8_t http_www[] = "http://www.";
    static const uint8_t https[] = "https://";
    static const uint8_t http[] = "http://";
    static const struct {
        uint8_t code;
        const uint8_t *text;
        uint8_t n;
    } prefs[] = {
        {0x02, https_www, 12},
        {0x01, http_www, 11},
        {0x04, https, 8},
        {0x03, http, 7},
    };
    uint8_t i;

    *code = 0;
    *skip = 0;
    for (i = 0; i < 4u; i++) {
        if (len > prefs[i].n && memcmp(buf, prefs[i].text, prefs[i].n) == 0) {
            *code = prefs[i].code;
            *skip = prefs[i].n;
            return 1;
        }
    }
    return 0;
}

/* Type-2 container: optional 00 bytes, then 03 <len|FF be16> message, optional FE.
 * A buffer that is not that container is kept as a bare message.
 */
static int ndef_from_tlv(uint8_t *dst, uint16_t *out_len, const uint8_t *buf, uint16_t len)
{
    uint16_t p = 0;
    uint16_t mlen;

    while (p < len && buf[p] == 0x00u) {
        p++;
    }
    if (p >= len) {
        return 0;
    }
    if (buf[p] != 0x03u) {
        if (len == 0 || len > OEPL_NDEF_MAX) {
            return 0;
        }
        memcpy(dst, buf, len);
        *out_len = len;
        return 1;
    }
    p++;
    if (p >= len) {
        return 0;
    }
    if (buf[p] == 0xFFu) {
        if ((uint16_t)(p + 3u) > len) {
            return 0;
        }
        mlen = (uint16_t)(((uint16_t)buf[p + 1u] << 8) | buf[p + 2u]);
        p = (uint16_t)(p + 3u);
    } else {
        mlen = buf[p];
        p++;
    }
    if (mlen == 0 || mlen > OEPL_NDEF_MAX || (uint32_t)p + mlen > len) {
        return 0;
    }
    memcpy(dst, buf + p, mlen);
    *out_len = mlen;
    return 1;
}

static int ndef_from_url(uint8_t *dst, uint16_t *out_len, const uint8_t *buf, uint16_t len)
{
    uint8_t code = 0;
    uint16_t skip = 0;
    uint16_t suffix;
    uint16_t total;

    while (len != 0 && (buf[len - 1u] == 0u || buf[len - 1u] == '\r'
                        || buf[len - 1u] == '\n' || buf[len - 1u] == ' ')) {
        len--;
    }
    if (len == 0) {
        return 0;
    }
    uri_prefix(buf, len, &code, &skip);
    suffix = (uint16_t)(len - skip);
    /* SR payload is one length byte: identifier code plus the rest. */
    if (suffix > 254u) {
        return 0;
    }
    total = (uint16_t)(5u + suffix);
    if (total > OEPL_NDEF_MAX) {
        return 0;
    }
    dst[0] = 0xD1;
    dst[1] = 0x01;
    dst[2] = (uint8_t)(1u + suffix);
    dst[3] = 'U';
    dst[4] = code;
    if (suffix != 0) {
        memcpy(dst + 5, buf + skip, suffix);
    }
    *out_len = total;
    return 1;
}

/* BSS, not the BLE stack. A failed conversion leaves the stored message alone. */
static uint8_t ndef_built[OEPL_NDEF_MAX];

/* 1 when buf becomes a bare NDEF message in the session. The previous message stays on failure. */
static int ndef_set(oepl_session *session, uint8_t src, const uint8_t *buf, uint16_t len)
{
    uint8_t *built = ndef_built;
    uint16_t n = 0;

    if (session == 0 || buf == 0 || len == 0 || len > OEPL_NDEF_MAX) {
        return 0;
    }
    if (src == OEPL_NDEF_RAW) {
        if (!ndef_from_tlv(built, &n, buf, len)) {
            return 0;
        }
    } else if (src == OEPL_NDEF_URL) {
        if (!ndef_from_url(built, &n, buf, len)) {
            return 0;
        }
    } else if (src == OEPL_NDEF_MSG) {
        memcpy(built, buf, len);
        n = len;
    } else {
        return 0;
    }
    if (n == 0 || n > OEPL_NDEF_MAX) {
        return 0;
    }
    memcpy(session->ndef_msg, built, n);
    session->ndef_len = n;
    session->ndef_src = src;
    session->ndef_dirty = 1;
    return 1;
}

static void ndef_clear(oepl_session *session)
{
    session->ndef_len = 0;
    session->ndef_src = 0;
    session->ndef_dirty = 1;
}

static void ndef_cmds(oepl_session *session, int enable)
{
    uint8_t next = session->ndef_flags;

    if (enable) {
        next = (uint8_t)(next & (uint8_t)~OEPL_NDEF_FLAG_NO_CMDS);
    } else {
        next = (uint8_t)(next | OEPL_NDEF_FLAG_NO_CMDS);
    }
    if (next == session->ndef_flags && session->ndef_dirty == 0) {
        return;
    }
    session->ndef_flags = next;
    session->ndef_dirty = 1;
}

/* The first firmware block is checked before anything is erased. A wrong
 * file must not wipe the stored picture. The payload CRC still runs later,
 * once every byte has been staged.
 */
static int fw_first_block_ok(const oepl_session *session, const uint8_t *file, uint32_t chunk)
{
    oepl_fw_view view;

    if (!oepl_fw_open(file, chunk, session->xfer_total, &view)) {
        return 0;
    }
    if (chunk < view.image_off + OEPL_FW_HEADER + 8u) {
        return 1;
    }
    return oepl_fw_vectors(file + view.image_off + OEPL_FW_HEADER);
}

static uint16_t finish_or_reject_block(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    uint32_t offset;

    if (!block_bytes_ok(session)) {
        if (session->block_bad != 0xFFFFu) {
            session->block_bad++;
        }
        session->block_bad_size = read_le16(session->body);
        session->block_bad_sum = read_le16(session->body + 2);
        begin_block(session);
        return reply_block(session, rsp, rsp_cap);
    }
    if (session->xfer_type == OEPL_TYPE_ZLIB) {
        return zlib_finish_block(session, rsp, rsp_cap);
    }
    if (ndef_xfer(session->xfer_type)) {
        /* One block holds the whole record. It must not go through the picture store. */
        if (session->chunk != session->xfer_size
            || !ndef_set(session, session->xfer_type,
                         session->body + OEPL_BLOCK_HDR, (uint16_t)session->chunk)) {
            return fail_transfer(session, rsp, rsp_cap);
        }
        session->active = 0;
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_SHOW);
    }
    offset = (uint32_t)session->block_id * OEPL_BLOCK_IMAGE;
    if (session->xfer_type == OEPL_TYPE_FW && offset == 0u
        && !fw_first_block_ok(session, session->body + OEPL_BLOCK_HDR, session->chunk)) {
        return fail_transfer(session, rsp, rsp_cap);
    }
    if (session->store != 0) {
        int st = session->store(session->store_user, offset,
                                session->body + OEPL_BLOCK_HDR,
                                (uint16_t)session->chunk);

        /* The flash erase or the page program has to return to the kernel.
         * The block stays in body[] and the access point is not told yet.
         */
        if (st == 2) {
            session->store_more = 1;
            return 0;
        }
        if (st != 0) {
            session->store_more = 0;
            begin_block(session);
            return reply_block(session, rsp, rsp_cap);
        }
    }
    session->store_more = 0;
    session->xfer_size -= session->chunk;
    if (session->xfer_size != 0) {
        session->block_id = (uint8_t)(session->block_id + 1u);
        begin_block(session);
        return reply_block(session, rsp, rsp_cap);
    }
    if (session->xfer_type == OEPL_TYPE_FW) {
        return mark_firmware(session, rsp, rsp_cap);
    }
    return mark_shown(session, rsp, rsp_cap);
}

static uint16_t handle_avail(oepl_session *session, const uint8_t *payload, uint16_t len,
                             uint8_t *rsp, uint16_t rsp_cap)
{
    uint64_t ver;
    uint32_t size;
    uint32_t expect = 0;
    uint8_t type;
    uint8_t arg;
    oepl_panel panel;

    if (len == 24u) {
        /* ESP32 leaves 7 bytes between the checksum and the version. */
        if (payload[0] != 0xFFu && oepl_checksum_of(payload, 24) != payload[0]) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
        }
        ver = read_le64(payload + 8);
        size = read_le32(payload + 16);
        type = payload[20];
        arg = payload[21];
        session->wide_req = 1u;
    } else if (len == OEPL_AVAIL_BYTES && avail_checksum_ok(payload)) {
        ver = read_le64(payload + 1);
        size = read_le32(payload + 9);
        type = payload[13];
        arg = payload[14];
        session->wide_req = 0u;
    } else {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
    }
    panel.width = session->width;
    panel.height = session->height;
    panel.colors = session->colors;
    if (type == OEPL_TYPE_ZLIB) {
        if (!zlib_size_ok(session, size)) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
        }
    } else if (type == OEPL_TYPE_FW) {
        if (size < OEPL_FW_MIN || size > OEPL_FW_MAX) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
        }
    } else if (ndef_xfer(type)) {
        if (size == 0) {
            if (session->active) {
                return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
            }
            ndef_clear(session);
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_SHOW);
        }
        if (size > OEPL_NDEF_MAX) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_FILE_BIG);
        }
    } else if (!oepl_expected_size(&panel, type, &expect) || size != expect || size == 0) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
    }
    /* Firmware and NDEF repeat. That must not look like a picture already shown. */
    if (type != OEPL_TYPE_FW && !ndef_xfer(type) && session->has_shown && !session->active
        && ver == session->shown_ver) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ALREADY);
    }
    if (session->active && ver == session->xfer_ver && type == session->xfer_type) {
        if (session->store_more) {
            return 0;
        }
        return reply_block(session, rsp, rsp_cap);
    }
    session->store_more = 0;
    session->xfer_ver = ver;
    session->xfer_size = size;
    session->xfer_total = size;
    session->xfer_type = type;
    session->xfer_arg = arg;
    session->show_type = type;
    session->block_id = 0;
    session->active = 1;
    if (type == OEPL_TYPE_ZLIB) {
        z_reset(session);
        oepl_inflate_reset();
        session->show_type = OEPL_TYPE_RAW_1BPP;
    } else if (type == OEPL_TYPE_FW || ndef_xfer(type)) {
        session->show_type = 0;
    }
    begin_block(session);
    return reply_block(session, rsp, rsp_cap);
}

static int part_belongs(const uint8_t *payload, uint16_t len,
                       uint8_t expect_block, uint8_t parts_this)
{
    uint8_t padded[OEPL_PART_BYTES];

    if (payload == 0 || len < 3u) {
        return 0;
    }
    if (len > OEPL_PART_BYTES) {
        len = OEPL_PART_BYTES;
    }
    memset(padded, 0, sizeof padded);
    memcpy(padded, payload, len);
    if (!oepl_checksum_ok(padded, OEPL_PART_BYTES)) {
        return 0;
    }
    if (padded[1] != expect_block || padded[2] >= parts_this) {
        return 0;
    }
    return 1;
}

static uint16_t handle_part(oepl_session *session, const uint8_t *payload, uint16_t len,
                            uint8_t *rsp, uint16_t rsp_cap)
{
    uint16_t body_len;

    if (!session->active) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
    }
    body_len = (uint16_t)(OEPL_BLOCK_HDR + session->chunk);
    if (!part_belongs(payload, len, session->block_id, session->parts_this)
        || !oepl_reassemble_part(session->body, body_len, session->mask, payload, len)) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_PART_BAD);
    }
    if (oepl_mask_pending(session->mask, session->parts_this)) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_PART_OK);
    }
    return finish_or_reject_block(session, rsp, rsp_cap);
}

/* 93 bytes: screen type, 42-byte settings, then the four pinout blocks.
 * Pinouts stay off. Width, height and color count are this glass.
 */
#define OEPL_DYN_BYTES 93u

static uint16_t reply_dynamic(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    uint8_t *q;

    if (rsp == 0 || cap < (2u + OEPL_DYN_BYTES)) {
        return 0;
    }
    memset(rsp, 0, 2u + OEPL_DYN_BYTES);
    oepl_frame_put(rsp, OEPL_CMD_ACK_DYN);
    q = rsp + 2;
    q += 2; /* screen type, left 0 */
    write_le16(q, session->hw_type);
    q += 2;
    q += 2; /* screen functions */
    q += 1; /* width/height swapped on the air */
    q += 2; /* width/height swapped in the record */
    write_le16(q, session->height);
    q += 2;
    write_le16(q, session->width);
    q += 2;
    q += 2; /* height offset */
    q += 2; /* width offset */
    write_le16(q, session->colors);
    return (uint16_t)(2u + OEPL_DYN_BYTES);
}

/* [has_pin][locked][nfc commands on][idle][ceiling le16]. No PIN is stored. */
/* [has_pin][locked][ndef_cmds][idle_relock_s][ceiling le16] */
static uint16_t reply_protection(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    if (rsp == 0 || session == 0 || cap < 8u) {
        return 0;
    }
    memset(rsp, 0, 8);
    oepl_frame_put(rsp, OEPL_CMD_ACK_PROT);
    rsp[2] = (session->pin_len != 0u) ? 1u : 0u;
    rsp[3] = (session->pin_len != 0u && session->locked) ? 1u : 0u;
    rsp[4] = (session->ndef_flags & OEPL_NDEF_FLAG_NO_CMDS) ? 0u : 1u;
    /* Both timeouts only mean anything once a PIN exists. A tag without one
     * reports zeros rather than advertising a lock it does not have.
     */
    if (session->pin_len != 0u) {
        rsp[5] = (uint8_t)OEPL_PIN_IDLE_S;
        write_le16(rsp + 6, (uint16_t)OEPL_PIN_CEIL_S);
    }
    return 8;
}

void oepl_session_abort(oepl_session *session)
{
    if (session == 0) {
        return;
    }
    session->active = 0;
    session->store_more = 0;
    session->z_more = 0;
    session->z_planes = 0;
    session->show_pending = 0;
    session->fw_pending = 0;
    session->block_id = 0;
    session->xfer_size = 0;
    /* xfer_ver is left as it was. A host that comes back with the same
     * version is offering the same picture, and that is a fresh transfer
     * rather than a resume - has_shown still decides whether it is needed.
     */
    memset(session->mask, 0, sizeof session->mask);
}

void oepl_pin_lock(oepl_session *session)
{
    if (session == 0 || session->pin_len == 0u) {
        return;
    }
    session->locked = 1;
    session->pin_idle = 0;
    session->pin_age = 0;
}

void oepl_pin_touch(oepl_session *session)
{
    if (session == 0) {
        return;
    }
    session->pin_idle = 0;
}

void oepl_pin_tick(oepl_session *session, uint16_t seconds)
{
    uint32_t idle;
    uint32_t age;

    if (session == 0 || session->pin_len == 0u || session->locked) {
        return;
    }
    /* Saturating, so a long gap between ticks cannot wrap a counter past its
     * limit and hand out an unlock that should have expired.
     */
    idle = (uint32_t)session->pin_idle + (uint32_t)seconds;
    age = (uint32_t)session->pin_age + (uint32_t)seconds;
    session->pin_idle = (idle > 0xFFFFu) ? 0xFFFFu : (uint16_t)idle;
    session->pin_age = (age > 0xFFFFu) ? 0xFFFFu : (uint16_t)age;
    if (session->pin_idle >= (uint16_t)OEPL_PIN_IDLE_S
        || session->pin_age >= (uint16_t)OEPL_PIN_CEIL_S) {
        oepl_pin_lock(session);
    }
}

int oepl_pin_allows(const oepl_session *session, uint16_t cmd)
{
    if (session == 0) {
        return 0;
    }
    if (session->pin_len == 0u || !session->locked) {
        return 1;
    }
    return cmd == OEPL_CMD_PROT || cmd == OEPL_CMD_LOCK;
}

/* 0x001E. A payload sets the PIN, nothing removes it.
 *
 * Setting one leaves the link that set it open: locking there would mean
 * typing the PIN twice to do anything, and "Lock now" exists for the case
 * where that is what was wanted. The lock arrives on its own at the idle
 * timeout, the ceiling, or the end of the link.
 */
static uint16_t handle_set_pin(oepl_session *session,
                               const uint8_t *arg, uint16_t arg_len,
                               uint8_t *rsp, uint16_t cap)
{
    if (arg_len == 0u) {
        session->pin_len = 0;
        session->locked = 0;
        session->pin_idle = 0;
        session->pin_age = 0;
        memset(session->pin, 0, sizeof session->pin);
        return reply_cmd(rsp, cap, OEPL_CMD_ACK_CMD);
    }
    if (arg_len > OEPL_PIN_MAX) {
        return reply_cmd(rsp, cap, OEPL_CMD_ERR);
    }
    memset(session->pin, 0, sizeof session->pin);
    memcpy(session->pin, arg, arg_len);
    session->pin_len = (uint8_t)arg_len;
    session->locked = 0;
    session->pin_idle = 0;
    session->pin_age = 0;
    return reply_cmd(rsp, cap, OEPL_CMD_ACK_CMD);
}

/* 0x001F. A payload offers a PIN, nothing locks at once. */
static uint16_t handle_lock(oepl_session *session,
                            const uint8_t *arg, uint16_t arg_len,
                            uint8_t *rsp, uint16_t cap)
{
    if (arg_len == 0u) {
        oepl_pin_lock(session);
        return reply_cmd(rsp, cap, OEPL_CMD_ACK_CMD);
    }
    if (session->pin_len == 0u) {
        /* Nothing to unlock. Answering ACK here would tell a caller its PIN
         * was accepted by a tag that has none.
         */
        return reply_cmd(rsp, cap, OEPL_CMD_ERR);
    }
    /* Length first, then every byte, and the whole stored length is compared
     * either way: a wrong PIN costs the same work as a right one, so the
     * answer carries nothing about how much of it was correct.
     */
    {
        uint8_t diff = (uint8_t)(session->pin_len ^ (uint8_t)arg_len);
        uint16_t i;

        for (i = 0; i < OEPL_PIN_MAX; i++) {
            uint8_t got = (i < arg_len) ? arg[i] : 0u;

            diff |= (uint8_t)(session->pin[i] ^ got);
        }
        if (diff != 0u) {
            return reply_cmd(rsp, cap, OEPL_CMD_ERR);
        }
    }
    session->locked = 0;
    session->pin_idle = 0;
    session->pin_age = 0;
    return reply_cmd(rsp, cap, OEPL_CMD_ACK_CMD);
}

/* The page reads the first six payload bytes big-endian: battery, raw ADC,
 * ADC temperature, panel temperature.
 */
static uint16_t reply_status(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    if (rsp == 0 || cap < 8u) {
        return 0;
    }
    memset(rsp, 0, 8);
    oepl_frame_put(rsp, OEPL_CMD_ACK_STATUS);
    rsp[2] = (uint8_t)(session->battery_mv >> 8);
    rsp[3] = (uint8_t)session->battery_mv;
    rsp[4] = (uint8_t)(session->battery_raw >> 8);
    rsp[5] = (uint8_t)session->battery_raw;
    rsp[6] = (uint8_t)session->temperature;
    rsp[7] = (uint8_t)session->temperature;
    return 8;
}

/* Two color planes, the 24-byte image header and 256 spare bytes, rounded
 * up to a 4 KiB page. 880x528 is 118784 bytes. The 128 KiB window holds one.
 */
static uint32_t image_each(const oepl_session *session)
{
    uint32_t pixels;
    uint32_t colors;
    uint32_t each;

    pixels = (uint32_t)session->width * (uint32_t)session->height;
    if (session->width == 0u || (pixels / 8u) == 0u) {
        return 131072u;
    }
    colors = session->colors > 2u ? session->colors : 2u;
    each = (pixels / 8u) * colors + 24u + 0x100u;
    return (each + 4095u) & ~4095u;
}

/* Field order matches the uploader's SYSINFO_FIELDS. Byte 1 is the length
 * of that fixed part, and the build stamp follows it.
 */
static uint16_t reply_sysinfo(const oepl_session *session, uint8_t *rsp, uint16_t cap)
{
    uint8_t *rec;
    uint8_t *q;
    static const char build[] = "ATC_OEPL";

    if (rsp == 0 || cap < 130u) {
        return 0;
    }
    memset(rsp, 0, 130);
    oepl_frame_put(rsp, OEPL_CMD_ACK_SYS);
    rec = rsp + 2;
    q = rec;
    *q++ = 1;
    *q++ = 0;
    write_le16(q, session->fw_version);
    q += 2;
    q += 2; /* capabilities */
    q += 2; /* battery */
    q += 2; /* adc raw */
    q += 1; /* adc temp */
    q += 1; /* epd temp */
    q += 4; /* uptime */
    q += 4; /* unix time */
    q += 1; /* wakeup */
    q += 1; /* panel state */
    q += 2; /* hw type */
    write_le16(q, session->screen_type);
    q += 2;
    q += 2; /* screen functions */
    write_le16(q, session->width);
    q += 2;
    write_le16(q, session->height);
    q += 2;
    *q++ = session->colors;
    q += 1; /* rotation */
    *q++ = 0; /* current slot: this glass has one, and it is the one on the panel */
    *q++ = 1;
    write_le32(q, image_each(session));
    q += 4;
    write_le32(q, 0x00A14013u);
    q += 4;
    write_le32(q, 524288u);
    q += 4;
    q += 2; /* flash status */
    q += 4; /* firmware size */
    write_le32(q, 0x00060000u);
    q += 4;
    write_le32(q, 131072u);
    q += 4;
    q += 4; /* kind, caps, device, address length: no chip has answered */
    write_le16(q, session->ndef_len);
    q += 2;
    q += 1; /* uid length */
    q += 7; /* uid */
    *q++ = session->ndef_src;
    *q++ = (session->ndef_flags & OEPL_NDEF_FLAG_NO_CMDS) ? 0u : 1u;
    q += 2; /* session, field */
    q += 1; /* no second radio */
    *q++ = 1;
    /* Advertising interval in 0.625 ms slots. 3200 slots is 2 s. */
    write_le32(q, session->adv_slots != 0u ? session->adv_slots : 3200u);
    q += 4;
    q += 4; /* reserved interval */
    *q++ = (session->pin_len != 0u) ? 1u : 0u;
    *q++ = (session->pin_len != 0u && session->locked) ? 1u : 0u;
    /* The idle timeout, which is the one a reader can act on. A byte, so the
     * ceiling does not fit here; 0x0020 reports that one.
     */
    *q++ = (session->pin_len != 0u) ? (uint8_t)OEPL_PIN_IDLE_S : 0u;
    q += 6; /* ble address, filled below */
    memcpy(q, session->mac8, 8);
    q += 8;
    write_le16(rec + 4, session->capabilities);
    write_le16(rec + 6, session->battery_mv);
    write_le16(rec + 8, session->battery_raw);
    rec[10] = (uint8_t)session->temperature;
    rec[11] = (uint8_t)session->temperature;
    write_le16(rec + 22, session->hw_type);
    memcpy(rec + 93, session->mac, 6);
    rec[1] = (uint8_t)(q - rec);
    *q++ = (uint8_t)(sizeof build - 1u);
    memcpy(q, build, sizeof build - 1u);
    q += sizeof build - 1u;
    return (uint16_t)(2u + (uint16_t)(q - rec));
}

uint16_t oepl_crc16_ccitt(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    uint8_t b;

    if (data == 0) {
        return crc;
    }
    while (len-- != 0u) {
        crc ^= (uint16_t)((uint16_t)*data++ << 8);
        for (b = 0; b < 8u; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t oepl_image_crc(uint64_t ver)
{
    uint8_t bytes[8];
    uint8_t i;

    for (i = 0; i < 8u; i++) {
        bytes[i] = (uint8_t)(ver >> (8u * i));
    }
    return oepl_crc16_ccitt(bytes, 8u);
}

uint8_t oepl_wake_byte(uint8_t counter, uint8_t reason)
{
    return (uint8_t)(((uint8_t)(counter & 0x0Fu) << 4) | (uint8_t)(reason & 0x0Fu));
}

uint8_t oepl_adv_payload(uint8_t *dst, uint8_t cap,
                         uint16_t hw_type, uint16_t fw_version,
                         uint16_t capabilities, uint16_t battery_mv,
                         int8_t temperature, uint8_t counter,
                         uint16_t img_crc, uint8_t wake_reason)
{
    if (dst == 0 || cap < 18u) {
        return 0;
    }
    dst[0] = 17;                 /* the type plus 16 bytes of manufacturer data */
    dst[1] = 0xFF;
    dst[2] = 0x37;
    dst[3] = 0x13;
    dst[4] = 2;
    dst[5] = (uint8_t)hw_type;
    dst[6] = (uint8_t)(hw_type >> 8);
    dst[7] = (uint8_t)fw_version;
    dst[8] = (uint8_t)(fw_version >> 8);
    dst[9] = (uint8_t)capabilities;
    dst[10] = (uint8_t)(capabilities >> 8);
    dst[11] = (uint8_t)battery_mv;
    dst[12] = (uint8_t)(battery_mv >> 8);
    dst[13] = (uint8_t)temperature;
    dst[14] = counter;
    dst[15] = (uint8_t)img_crc;
    dst[16] = (uint8_t)(img_crc >> 8);
    dst[17] = wake_reason;
    return 18u;
}

uint8_t oepl_scan_payload(uint8_t *dst, uint8_t cap,
                          const uint8_t *name, uint8_t name_len)
{
    uint8_t total;

    if (dst == 0 || name == 0 || name_len == 0u || name_len > 18u) {
        return 0;
    }
    total = (uint8_t)(6u + name_len);
    if (cap < total) {
        return 0;
    }
    dst[0] = 3;                  /* the 16-bit service UUID */
    dst[1] = 0x03;
    dst[2] = 0x37;
    dst[3] = 0x13;
    dst[4] = (uint8_t)(name_len + 1u);
    dst[5] = 0x09;               /* the complete local name */
    memcpy(dst + 6, name, name_len);
    return total;
}

int oepl_fw_open(const uint8_t *head, uint32_t head_len, uint32_t file_len, oepl_fw_view *out)
{
    uint32_t off;
    uint32_t code_size;
    uint32_t crc;

    if (head == 0 || out == 0 || head_len < 12u || file_len < OEPL_FW_MIN || file_len > OEPL_FW_MAX) {
        return 0;
    }
    /* Not this boot image. Leave the bank alone. */
    if (head[8] == 0x4Bu && head[9] == 0x4Eu && head[10] == 0x4Cu && head[11] == 0x54u) {
        return 0;
    }
    if (head_len >= OEPL_FW_HEADER && head[0] == 0x70u && head[1] == 0x51u) {
        off = 0;
    } else if (head_len >= (OEPL_FW_PREFIX + OEPL_FW_HEADER)
               && head[4] == 0x93u && head[5] == 0x27u
               && head[8] == 0x70u && head[9] == 0x51u) {
        off = OEPL_FW_PREFIX;
    } else {
        return 0;
    }
    if (head_len < off + OEPL_FW_HEADER) {
        return 0;
    }
    if (head[off + 2u] != 0xAAu || head[off + 3u] != 0x00u || head[off + 36u] != 0x00u) {
        return 0;
    }
    /* Version string at byte 12 of the boot header. Anything else is the wrong file. */
    if (head[off + 12u] != 'A' || head[off + 13u] != 'T' || head[off + 14u] != 'C'
        || head[off + 15u] != '_' || head[off + 16u] != 'O' || head[off + 17u] != 'E'
        || head[off + 18u] != 'P' || head[off + 19u] != 'L') {
        return 0;
    }
    code_size = read_le32(head + off + 4u);
    crc = read_le32(head + off + 8u);
    if (code_size < 8u || (OEPL_FW_HEADER + code_size) > OEPL_FW_BANK) {
        return 0;
    }
    if (file_len != off + OEPL_FW_HEADER + code_size) {
        return 0;
    }
    out->image_off = off;
    out->image_len = OEPL_FW_HEADER + code_size;
    out->code_size = code_size;
    out->crc = crc;
    return 1;
}

int oepl_fw_vectors(const uint8_t vectors[8])
{
    uint32_t sp;
    uint32_t reset;

    if (vectors == 0) {
        return 0;
    }
    sp = read_le32(vectors);
    reset = read_le32(vectors + 4);
    if (sp < 0x07FC0000u || sp >= 0x07FD8000u) {
        return 0;
    }
    if ((reset & 1u) == 0u) {
        return 0;
    }
    reset &= ~1u;
    if (reset < 0x07FC0000u || reset >= 0x07FD8000u) {
        return 0;
    }
    return 1;
}

void oepl_session_init(oepl_session *session, const oepl_panel *panel,
                       oepl_store_fn store, void *user)
{
    memset(session, 0, sizeof *session);
    if (panel != 0) {
        session->width = panel->width;
        session->height = panel->height;
        session->colors = panel->colors;
    }
    session->store = store;
    session->store_user = user;
    session->adv_slots = 3200u;
}

uint16_t oepl_session_zlib_poll(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    if (session == 0 || !session->z_more) {
        return 0;
    }
    return zlib_finish_block(session, rsp, rsp_cap);
}

uint16_t oepl_session_store_poll(oepl_session *session, uint8_t *rsp, uint16_t rsp_cap)
{
    if (session == 0 || !session->store_more) {
        return 0;
    }
    return finish_or_reject_block(session, rsp, rsp_cap);
}

uint16_t oepl_session_handle(oepl_session *session,
                             const uint8_t *req, uint16_t req_len,
                             uint8_t *rsp, uint16_t rsp_cap)
{
    uint16_t cmd;

    if (session == 0 || req == 0 || req_len < 2u) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
    }
    cmd = oepl_frame_get(req);
    /* The gate again, although the firmware has already applied it to every
     * command including its own. Two reasons to repeat it: this module is
     * also driven straight from the host tests, and a lock that depended on
     * every caller remembering to ask would eventually meet one that forgot.
     */
    if (!oepl_pin_allows(session, cmd)) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
    }
    oepl_pin_touch(session);
    if (cmd == OEPL_CMD_GET_INFOS) {
        return reply_infos(session, rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_AVAIL) {
        return handle_avail(session, req + 2, (uint16_t)(req_len - 2u), rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_BLOCK_PART) {
        return handle_part(session, req + 2, (uint16_t)(req_len - 2u), rsp, rsp_cap);
    }
    /* Sent around every block. An error here would be logged in the middle
     * of an upload, so answer and leave the transfer as it is.
     */
    if (cmd == OEPL_CMD_FAST || cmd == OEPL_CMD_SLOW) {
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ACK_CMD);
    }
    if (cmd == OEPL_CMD_PROT) {
        return reply_protection(session, rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_SET_PIN) {
        return handle_set_pin(session, req + 2, (uint16_t)(req_len - 2u),
                              rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_LOCK) {
        return handle_lock(session, req + 2, (uint16_t)(req_len - 2u),
                           rsp, rsp_cap);
    }
    /* Arming the stream is protocol state; the pushing is the firmware's job,
     * because only it owns the ring and the notification path.
     */
    if (cmd == OEPL_CMD_LOG) {
        session->log_stream = (req_len >= 3u && req[2] != 0u) ? 1u : 0u;
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ACK_CMD);
    }
    if (cmd == OEPL_CMD_SET_NDEF || cmd == OEPL_CMD_CLEAR_NDEF
        || cmd == OEPL_CMD_NFC_OFF || cmd == OEPL_CMD_NFC_ON) {
        if (session->active) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
        }
        if (cmd == OEPL_CMD_CLEAR_NDEF) {
            ndef_clear(session);
        } else if (cmd == OEPL_CMD_NFC_OFF) {
            ndef_cmds(session, 0);
        } else if (cmd == OEPL_CMD_NFC_ON) {
            ndef_cmds(session, 1);
        } else if (req_len < 4u
                   || !ndef_set(session, req[2], req + 3, (uint16_t)(req_len - 3u))) {
            return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
        }
        return reply_cmd(rsp, rsp_cap, OEPL_CMD_ACK_CMD);
    }
    if (cmd == OEPL_CMD_GET_NDEF) {
        uint16_t off = 0;
        uint16_t total = session->ndef_len;
        uint16_t n = 0;
        uint16_t room;

        if (rsp == 0 || rsp_cap < 7u) {
            return 0;
        }
        if (req_len >= 4u) {
            off = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
        }
        room = (uint16_t)(rsp_cap - 7u);
        if (room > OEPL_NDEF_CHUNK) {
            room = OEPL_NDEF_CHUNK;
        }
        if (off < total) {
            n = (uint16_t)(total - off);
            if (n > room) {
                n = room;
            }
        }
        memset(rsp, 0, (uint16_t)(7u + n));
        oepl_frame_put(rsp, OEPL_CMD_ACK_NDEF);
        rsp[2] = session->ndef_src;
        rsp[3] = (uint8_t)total;
        rsp[4] = (uint8_t)(total >> 8);
        rsp[5] = (uint8_t)off;
        rsp[6] = (uint8_t)(off >> 8);
        if (n != 0) {
            memcpy(rsp + 7, session->ndef_msg + off, n);
        }
        return (uint16_t)(7u + n);
    }
    if (cmd == OEPL_CMD_DYN_READ) {
        return reply_dynamic(session, rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_SYSINFO) {
        return reply_sysinfo(session, rsp, rsp_cap);
    }
    if (cmd == OEPL_CMD_DEBUG_STS) {
        return reply_status(session, rsp, rsp_cap);
    }
    return reply_cmd(rsp, rsp_cap, OEPL_CMD_ERR);
}

void oepl_session_missing(const oepl_session *session, uint8_t mask[OEPL_MASK_BYTES])
{
    if (session == 0 || mask == 0) {
        return;
    }
    memcpy(mask, session->mask, OEPL_MASK_BYTES);
}
