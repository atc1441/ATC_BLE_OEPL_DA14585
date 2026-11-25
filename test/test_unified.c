/* Host tests for the parts of this firmware that do not touch the chip.
 *
 * Three modules are written so they can be built for the host as well as for
 * the DA14585: the protocol (src/oepl/oepl_session.c), the settings record
 * (src/app/settings.c) and the panel catalog (src/app/panel.c). None of them
 * includes an SDK header. That is deliberate, and this file is the reason:
 * the PIN, the lock gate and the stored address are the places where a
 * mistake is expensive and hard to see on a tag, so they are tested where
 * the test can be run in a second.
 *
 * Build and run with: python tools/test.py
 */

#include "oepl_session.h"
#include "panel.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* One command in, the notification opcode out. */
static uint16_t call(oepl_session *s, const uint8_t *req, uint16_t len)
{
    uint8_t rsp[256];
    uint16_t n = oepl_session_handle(s, req, len, rsp, sizeof rsp);

    if (n < 2u) {
        return 0;
    }
    return oepl_frame_get(rsp);
}

static uint16_t call_rsp(oepl_session *s, const uint8_t *req, uint16_t len,
                         uint8_t *rsp, uint16_t cap, uint16_t *out_len)
{
    uint16_t n = oepl_session_handle(s, req, len, rsp, cap);

    *out_len = n;
    if (n < 2u) {
        return 0;
    }
    return oepl_frame_get(rsp);
}

/* A bare two-byte command. */
static uint16_t cmd0(oepl_session *s, uint16_t cmd)
{
    uint8_t req[2];

    oepl_frame_put(req, cmd);
    return call(s, req, 2);
}

/* A command with an ASCII payload, which is how a PIN travels. */
static uint16_t cmd_text(oepl_session *s, uint16_t cmd, const char *text)
{
    uint8_t req[2 + OEPL_PIN_MAX + 8];
    uint16_t n = (uint16_t)strlen(text);

    oepl_frame_put(req, cmd);
    memcpy(req + 2, text, n);
    return call(s, req, (uint16_t)(2u + n));
}

static void session_start(oepl_session *s)
{
    oepl_session_init(s, &oepl_panel_zkc75s, 0, 0);
}

/* ---- the PIN ----------------------------------------------------------- */

static void test_pin_basics(void)
{
    oepl_session s;

    session_start(&s);
    ok(s.pin_len == 0 && s.locked == 0, "a fresh session has no PIN");
    ok(oepl_pin_allows(&s, OEPL_CMD_AVAIL), "no PIN allows everything");

    /* Locking a tag that has no PIN must not lock it: there would be no way
     * back in.
     */
    ok(cmd0(&s, OEPL_CMD_LOCK) == OEPL_CMD_ACK_CMD, "lock without a PIN acks");
    ok(s.locked == 0, "lock without a PIN does not lock");
    ok(oepl_pin_allows(&s, OEPL_CMD_AVAIL), "still open without a PIN");

    /* Offering a PIN to a tag that has none is refused rather than acked:
     * an ack would tell the caller its PIN was accepted.
     */
    ok(cmd_text(&s, OEPL_CMD_LOCK, "1234") == OEPL_CMD_ERR,
       "unlock against no PIN is refused");

    ok(cmd_text(&s, OEPL_CMD_SET_PIN, "1234") == OEPL_CMD_ACK_CMD, "set a PIN");
    ok(s.pin_len == 4, "PIN length stored");
    ok(s.locked == 0, "setting a PIN leaves this link open");
    ok(oepl_pin_allows(&s, OEPL_CMD_AVAIL), "open right after being set");
}

static void test_pin_gate(void)
{
    oepl_session s;

    session_start(&s);
    cmd_text(&s, OEPL_CMD_SET_PIN, "secret");
    ok(cmd0(&s, OEPL_CMD_LOCK) == OEPL_CMD_ACK_CMD, "lock now");
    ok(s.locked == 1, "locked");

    /* Exactly two commands get through a locked tag. */
    ok(oepl_pin_allows(&s, OEPL_CMD_PROT), "locked still reports protection");
    ok(oepl_pin_allows(&s, OEPL_CMD_LOCK), "locked still takes a PIN");
    ok(!oepl_pin_allows(&s, OEPL_CMD_AVAIL), "locked refuses a transfer");
    ok(!oepl_pin_allows(&s, OEPL_CMD_SYSINFO), "locked refuses sysinfo");
    ok(!oepl_pin_allows(&s, OEPL_CMD_GET_INFOS), "locked refuses get_infos");
    ok(!oepl_pin_allows(&s, OEPL_CMD_SET_PIN), "locked refuses a new PIN");
    ok(!oepl_pin_allows(&s, OEPL_CMD_LOG), "locked refuses the log");
    ok(!oepl_pin_allows(&s, OEPL_CMD_SET_NDEF), "locked refuses NDEF");
    ok(!oepl_pin_allows(&s, 0x0004u), "locked refuses the glass");
    ok(!oepl_pin_allows(&s, 0x0009u), "locked refuses the address");
    ok(!oepl_pin_allows(&s, 0x0001u), "locked refuses the lamp");

    /* And the dispatcher honours it, not just the predicate. */
    ok(cmd0(&s, OEPL_CMD_SYSINFO) == OEPL_CMD_ERR, "locked sysinfo is FFFF");
    ok(cmd0(&s, OEPL_CMD_PROT) == OEPL_CMD_ACK_PROT, "locked protection answers");

    ok(cmd_text(&s, OEPL_CMD_LOCK, "wrong") == OEPL_CMD_ERR, "wrong PIN refused");
    ok(s.locked == 1, "wrong PIN leaves it locked");
    /* A prefix of the real PIN is still wrong. */
    ok(cmd_text(&s, OEPL_CMD_LOCK, "secre") == OEPL_CMD_ERR, "short prefix refused");
    ok(cmd_text(&s, OEPL_CMD_LOCK, "secrets") == OEPL_CMD_ERR, "longer refused");
    ok(s.locked == 1, "still locked");

    ok(cmd_text(&s, OEPL_CMD_LOCK, "secret") == OEPL_CMD_ACK_CMD, "right PIN unlocks");
    ok(s.locked == 0, "unlocked");
    ok(cmd0(&s, OEPL_CMD_SYSINFO) == OEPL_CMD_ACK_SYS, "sysinfo answers again");
}

static void test_pin_remove_and_max(void)
{
    oepl_session s;
    char long_pin[OEPL_PIN_MAX + 2];
    unsigned i;

    session_start(&s);
    cmd_text(&s, OEPL_CMD_SET_PIN, "abcd");
    ok(cmd0(&s, OEPL_CMD_SET_PIN) == OEPL_CMD_ACK_CMD, "bare 001E removes the PIN");
    ok(s.pin_len == 0 && s.locked == 0, "PIN gone");
    ok(cmd0(&s, OEPL_CMD_SYSINFO) == OEPL_CMD_ACK_SYS, "open after removal");

    /* The longest PIN that fits, and one byte more. */
    for (i = 0; i < OEPL_PIN_MAX; i++) {
        long_pin[i] = (char)('a' + (i % 26u));
    }
    long_pin[OEPL_PIN_MAX] = 0;
    ok(cmd_text(&s, OEPL_CMD_SET_PIN, long_pin) == OEPL_CMD_ACK_CMD,
       "a 16-byte PIN is accepted");
    ok(s.pin_len == OEPL_PIN_MAX, "16 bytes stored");
    cmd0(&s, OEPL_CMD_LOCK);
    ok(cmd_text(&s, OEPL_CMD_LOCK, long_pin) == OEPL_CMD_ACK_CMD,
       "a 16-byte PIN unlocks");

    long_pin[OEPL_PIN_MAX] = 'z';
    long_pin[OEPL_PIN_MAX + 1] = 0;
    ok(cmd_text(&s, OEPL_CMD_SET_PIN, long_pin) == OEPL_CMD_ERR,
       "17 bytes is refused");
    ok(s.pin_len == OEPL_PIN_MAX, "the refused PIN did not replace the old one");
}

static void test_pin_timeouts(void)
{
    oepl_session s;

    /* The idle window. */
    session_start(&s);
    cmd_text(&s, OEPL_CMD_SET_PIN, "pin");
    oepl_pin_tick(&s, (uint16_t)(OEPL_PIN_IDLE_S - 2u));
    ok(s.locked == 0, "still open just inside the idle window");
    oepl_pin_tick(&s, 2u);
    ok(s.locked == 1, "relocks at the idle window");

    /* Traffic holds it open - but only up to the ceiling. */
    session_start(&s);
    cmd_text(&s, OEPL_CMD_SET_PIN, "pin");
    {
        uint16_t elapsed = 0;

        /* A command every 10 s keeps the idle clock down. */
        while (elapsed < (uint16_t)(OEPL_PIN_CEIL_S - 20u)) {
            oepl_pin_tick(&s, 10u);
            elapsed = (uint16_t)(elapsed + 10u);
            ok(s.locked == 0, "traffic holds it open below the ceiling");
            oepl_pin_touch(&s);
        }
        oepl_pin_tick(&s, 10u);
        ok(s.locked == 0, "still open at 590 s");
        oepl_pin_tick(&s, 10u);
        ok(s.locked == 1, "the ceiling relocks it despite traffic");
    }

    /* A tag with no PIN is never locked by the clock. */
    session_start(&s);
    oepl_pin_tick(&s, 10000u);
    ok(s.locked == 0, "no PIN, no relock");

    /* A single huge tick must not wrap past the limit and hand out an
     * unlock that should have expired.
     */
    session_start(&s);
    cmd_text(&s, OEPL_CMD_SET_PIN, "pin");
    oepl_pin_tick(&s, 0xFFFFu);
    ok(s.locked == 1, "a saturating tick still locks");
}

static void test_protection_reply(void)
{
    oepl_session s;
    uint8_t rsp[32];
    uint16_t n = 0;
    uint8_t req[2];

    session_start(&s);
    oepl_frame_put(req, OEPL_CMD_PROT);

    ok(call_rsp(&s, req, 2, rsp, sizeof rsp, &n) == OEPL_CMD_ACK_PROT, "0020 answers 00D4");
    ok(n == 8, "protection is 6 payload bytes");
    /* [has_pin][locked][ndef_cmds][idle][ceiling le16] - the layout the
     * uploader parses.
     */
    ok(rsp[2] == 0, "no PIN");
    ok(rsp[3] == 0, "not locked");
    ok(rsp[4] == 1, "NDEF commands on by default");
    ok(rsp[5] == 0 && rsp[6] == 0 && rsp[7] == 0,
       "no timeouts advertised without a PIN");

    cmd_text(&s, OEPL_CMD_SET_PIN, "x");
    call_rsp(&s, req, 2, rsp, sizeof rsp, &n);
    ok(rsp[2] == 1, "PIN reported");
    ok(rsp[3] == 0, "open while this link holds it");
    ok(rsp[5] == (uint8_t)OEPL_PIN_IDLE_S, "idle window reported");
    ok((uint16_t)(rsp[6] | ((uint16_t)rsp[7] << 8)) == (uint16_t)OEPL_PIN_CEIL_S,
       "ceiling reported little-endian");

    oepl_pin_lock(&s);
    call_rsp(&s, req, 2, rsp, sizeof rsp, &n);
    ok(rsp[3] == 1, "locked reported");
}

/* ---- the log stream ---------------------------------------------------- */

static void test_log_stream(void)
{
    oepl_session s;
    uint8_t req[3];

    session_start(&s);
    ok(s.log_stream == 0, "not streaming to begin with");

    oepl_frame_put(req, OEPL_CMD_LOG);
    req[2] = 1;
    ok(call(&s, req, 3) == OEPL_CMD_ACK_CMD, "0022 01 acks");
    ok(s.log_stream == 1, "streaming armed");

    req[2] = 0;
    ok(call(&s, req, 3) == OEPL_CMD_ACK_CMD, "0022 00 acks");
    ok(s.log_stream == 0, "streaming stopped");

    /* A bare 0x0022 is "off" rather than a refusal: the one thing it must
     * never do is leave a tag streaming because a byte went missing.
     */
    req[2] = 1;
    call(&s, req, 3);
    ok(call(&s, req, 2) == OEPL_CMD_ACK_CMD, "bare 0022 acks");
    ok(s.log_stream == 0, "bare 0022 stops the stream");
}

/* ---- sysinfo ----------------------------------------------------------- */

static void test_sysinfo_layout(void)
{
    oepl_session s;
    uint8_t rsp[160];
    uint16_t n = 0;
    uint8_t req[2];
    static const uint8_t mac8[8] = {0x13, 0x37, 0xA0, 0xA3, 0xB8, 0x27, 0x9D, 0xFC};

    session_start(&s);
    s.screen_type = 1u;
    s.fw_version = 0x0030u;
    memcpy(s.mac8, mac8, 8);
    memcpy(s.mac, mac8 + 2, 6);
    oepl_frame_put(req, OEPL_CMD_SYSINFO);

    ok(call_rsp(&s, req, 2, rsp, sizeof rsp, &n) == OEPL_CMD_ACK_SYS, "0021 answers 00D5");

    /* Byte 1 is where the build stamp starts. The uploader adds up its own
     * field table and compares; 107 is that sum. If this moves, every field
     * after the change is read wrong by every client.
     */
    ok(rsp[2] == 1, "record version 1");
    ok(rsp[3] == 107, "fixed block is 107 bytes");
    ok(n == (uint16_t)(2u + 107u + 1u + 8u), "record is the block, a length and the stamp");
    ok(rsp[2 + 107] == 8, "stamp length byte");
    ok(memcmp(rsp + 2 + 108, "ATC_OEPL", 8) == 0, "stamp is ATC_OEPL");

    /* Field offsets inside the fixed block, counted from rsp+2. */
    ok((uint16_t)(rsp[2 + 2] | ((uint16_t)rsp[2 + 3] << 8)) == 0x0030u, "fw_version at 2");
    ok((uint16_t)(rsp[2 + 24] | ((uint16_t)rsp[2 + 25] << 8)) == 1u, "config id at 24");
    ok((uint16_t)(rsp[2 + 28] | ((uint16_t)rsp[2 + 29] << 8)) == 880u, "screen_w at 28");
    ok((uint16_t)(rsp[2 + 30] | ((uint16_t)rsp[2 + 31] << 8)) == 528u, "screen_h at 30");
    ok(rsp[2 + 32] == 2, "colors at 32");
    ok(rsp[2 + 34] == 0 && rsp[2 + 35] == 1, "one image slot, index 0");
    ok(memcmp(rsp + 2 + 93, mac8 + 2, 6) == 0, "ble_mac at 93");
    ok(memcmp(rsp + 2 + 99, mac8, 8) == 0, "oepl_mac at 99");

    /* The two temperature bytes are the same reading on purpose: this board
     * has one sensor, in the panel controller, and no separate ADC channel
     * for it. Both slots carry it rather than one of them carrying a zero.
     */
    s.temperature = 21;
    call_rsp(&s, req, 2, rsp, sizeof rsp, &n);
    ok((int8_t)rsp[2 + 10] == 21 && (int8_t)rsp[2 + 11] == 21,
       "both temperature bytes are the panel sensor");

    /* The protection fields travel here too. */
    ok(rsp[2 + 90] == 0 && rsp[2 + 91] == 0, "no PIN in sysinfo");
    cmd_text(&s, OEPL_CMD_SET_PIN, "k");
    call_rsp(&s, req, 2, rsp, sizeof rsp, &n);
    ok(rsp[2 + 90] == 1, "has_pin at 90");
    ok(rsp[2 + 91] == 0, "locked at 91");
    ok(rsp[2 + 92] == (uint8_t)OEPL_PIN_IDLE_S, "relock_s at 92");
}

/* ---- the settings record ----------------------------------------------- */

static void test_settings_roundtrip(void)
{
    atc_settings in;
    atc_settings out;
    uint8_t rec[ATC_SETTINGS_BYTES];
    static const uint8_t mac8[8] = {0x13, 0x37, 0xA0, 0xA3, 0xB8, 0x27, 0x9D, 0xFC};

    atc_settings_clear(&in);
    in.config_id = 1u;
    in.have_mac = 1;
    memcpy(in.mac, mac8, 8);
    atc_settings_encode(&in, rec);

    ok(atc_settings_decode(rec, &out), "a written record reads back");
    ok(out.config_id == 1u, "config id survives");
    ok(out.have_mac == 1, "the address flag survives");
    ok(memcmp(out.mac, mac8, 8) == 0, "the address survives");

    /* Every single-bit change in the record has to be caught, or a half-
     * written sector comes back as a plausible address.
     */
    {
        unsigned byte;
        unsigned bit;
        int caught = 1;

        for (byte = 0; byte < ATC_SETTINGS_BYTES; byte++) {
            for (bit = 0; bit < 8u; bit++) {
                uint8_t bad[ATC_SETTINGS_BYTES];

                memcpy(bad, rec, sizeof bad);
                bad[byte] ^= (uint8_t)(1u << bit);
                if (atc_settings_decode(bad, &out)) {
                    caught = 0;
                }
            }
        }
        ok(caught, "every one-bit change in the record is rejected");
    }
}

static void test_settings_blank(void)
{
    atc_settings out;
    uint8_t rec[ATC_SETTINGS_BYTES];

    /* An erased sector and a blank one both mean "nothing stored". */
    memset(rec, 0xFF, sizeof rec);
    ok(!atc_settings_decode(rec, &out), "an erased sector is not a record");
    memset(rec, 0x00, sizeof rec);
    ok(!atc_settings_decode(rec, &out), "a zeroed sector is not a record");

    /* A record from a layout this build does not know is refused rather than
     * read with the wrong field offsets.
     */
    {
        atc_settings in;

        atc_settings_clear(&in);
        in.config_id = 1u;
        atc_settings_encode(&in, rec);
        rec[4] = (uint8_t)(ATC_SETTINGS_VERSION + 1u);
        ok(!atc_settings_decode(rec, &out), "a future version is refused");
    }
}

static void test_settings_address(void)
{
    atc_settings in;
    uint8_t addr[6];
    static const uint8_t mac8[8] = {0x13, 0x37, 0xA0, 0xA3, 0xB8, 0x27, 0x9D, 0xFC};

    atc_settings_clear(&in);
    ok(!atc_settings_bd_addr(&in, addr), "no stored address, nothing to apply");

    in.have_mac = 1;
    memcpy(in.mac, mac8, 8);
    ok(atc_settings_bd_addr(&in, addr), "a stored address applies");
    /* bd_addr runs the other way round from the printed address. The tag
     * whose MAC is 1337A0A3B8279DFC advertises A0:A3:B8:27:9D:FC, so byte 0
     * of bd_addr is FC.
     */
    ok(addr[0] == 0xFC && addr[1] == 0x9D && addr[2] == 0x27
       && addr[3] == 0xB8 && addr[4] == 0xA3 && addr[5] == 0xA0,
       "the six low bytes, reversed");

    /* Neither of these is an address, and storing one would bring the tag up
     * unreachable with a programmer as the only way back.
     */
    memset(in.mac, 0x00, 8);
    ok(!atc_settings_bd_addr(&in, addr), "an all-zero address is refused");
    memset(in.mac, 0xFF, 8);
    ok(!atc_settings_bd_addr(&in, addr), "an all-ones address is refused");
}

/* ---- the panel catalog ------------------------------------------------- */

static void test_panel_catalog(void)
{
    const atc_panel *row;
    unsigned i;

    ok(atc_panel_count() >= 2u, "the catalog has the unconfigured row and a board");

    /* Row 0 is "unconfigured": id 0, no glass, no driver, nothing fitted.
     * It must not be usable, or a tag that was never configured would try
     * to draw.
     */
    row = atc_panel_at(0);
    ok(row != 0 && row->config_id == ATC_CFG_NONE, "row 0 is the unconfigured one");
    ok(row != 0 && !atc_panel_usable(row), "the unconfigured row is not usable");
    ok(row != 0 && row->width == 0u && row->height == 0u, "and names no geometry");
    ok(row != 0 && row->caps == 0u, "and carries no hardware");

    /* Every other row has to be one this build can actually drive. A row
     * that fails this would be offered and then drawn wrong.
     */
    for (i = 1u; i < atc_panel_count(); i++) {
        row = atc_panel_at(i);
        ok(row != 0, "row exists");
        ok(atc_panel_usable(row), "every configured row is usable");
        ok(row->config_id != ATC_CFG_NONE, "every configured row has an id");
        ok(row->hw_type != 0u, "every configured row has a hardware type");
        ok(row->name != 0, "every row has a name");
    }

    /* Panel ids and hardware types are both unique, or a lookup is a
     * coin toss.
     */
    {
        unsigned j;

        for (i = 0; i < atc_panel_count(); i++) {
            for (j = i + 1u; j < atc_panel_count(); j++) {
                ok(atc_panel_at(i)->config_id != atc_panel_at(j)->config_id,
                   "config ids are unique");
                ok(atc_panel_at(i)->hw_type != atc_panel_at(j)->hw_type,
                   "hardware types are unique");
            }
        }
    }

    ok(atc_panel_at(atc_panel_count()) == 0, "past the end is null");
    ok(atc_panel_find(0xFFFFu) == 0, "an unknown hardware type is null");
    ok(atc_panel_find(0u) == 0, "hardware type 0 is not a lookup");
    ok(atc_panel_find_config(0xFFFFu) == 0, "an unknown config id is null");
    ok(atc_panel_find_config(ATC_CFG_NONE) == 0, "config 0 selects no glass");

    /* A freshly started image is unconfigured until something selects. */
    row = atc_panel_get();
    ok(row != 0, "there is always a row");
    ok(row == atc_panel_at(0), "and it is the unconfigured one before any select");
    ok(!atc_panel_configured(), "so the tag reports itself unconfigured");
    ok(!atc_panel_has(ATC_CAP_LED) && !atc_panel_has(ATC_CAP_NFC),
       "and claims no hardware");

    ok(!atc_panel_select(0xFFFFu), "selecting an unknown id fails");
    ok(!atc_panel_select(ATC_CFG_NONE), "selecting 0 is not a selection");
    ok(atc_panel_get() == row, "a failed select leaves the config alone");

    /* Now configure it, the way a stored record or 0x0004 does. */
    row = atc_panel_at(1);
    ok(atc_panel_select(row->config_id), "selecting a known config works");
    ok(atc_panel_get() == row, "and that is the row in force");
    ok(atc_panel_configured(), "a configured tag says so");
    ok(atc_panel_find_config(row->config_id) == row, "findable by config id");
    ok(atc_panel_find(row->hw_type) == row, "findable by hardware type");

    /* The capability bits are what the board is, not what the firmware can
     * do: a board with no LED fitted must not claim one.
     */
    ok(atc_panel_has(ATC_CAP_LED) == ((row->caps & ATC_CAP_LED) != 0u),
       "the LED bit is reported from the row");
    ok(atc_panel_has(ATC_CAP_NFC) == ((row->caps & ATC_CAP_NFC) != 0u),
       "the NFC bit is reported from the row");
}

static void test_panel_limits(void)
{
    atc_panel p;

    p.config_id = 1u;
    p.hw_type = 0x0001u;
    p.colors = 2u;
    p.driver = ATC_DRV_ZKC75S;
    p.caps = 0u;
    p.boost = 0x40u;
    p.name = "test";

    p.width = 800u;
    p.height = 480u;
    ok(atc_panel_usable(&p), "800x480 two-plane fits");

    /* The driver holds one row at a time, 128 bytes of it. */
    p.width = ATC_PANEL_WIDTH_MAX;
    p.height = 8u;
    ok(atc_panel_usable(&p), "the widest row the driver can hold");
    p.width = ATC_PANEL_WIDTH_MAX + 8u;
    ok(!atc_panel_usable(&p), "wider than the driver's row is refused");

    /* A byte is eight pixels; the driver cannot clock a part of one. */
    p.width = 884u;
    p.height = 480u;
    ok(!atc_panel_usable(&p), "a width that is not a multiple of 8 is refused");
    p.width = 0u;
    ok(!atc_panel_usable(&p), "zero width is refused");
    p.width = 800u;
    p.height = 0u;
    ok(!atc_panel_usable(&p), "zero height is refused");

    /* Two planes is what a picture costs, whatever the glass. */
    p.width = 880u;
    p.height = 528u;
    ok(atc_panel_image_size(&p) == 116160u, "880x528 is 116160 bytes a picture");
    p.width = 960u;
    p.height = 680u;
    ok(atc_panel_image_size(&p) == 163200u, "960x680 is 163200");

    /* Where that picture goes depends on the chip, not on a constant. The
     * 512 KiB board keeps the window it has always had, and the 2 MiB board
     * gets one at the top of its own flash.
     */
    {
        uint32_t base = 0;
        uint32_t end = 0;

        p.width = 880u;
        p.height = 528u;
        ok(atc_panel_window(&p, 0x00080000u, &base, &end), "880x528 fits 512 KiB");
        ok(base == 0x00060000u && end == 0x00080000u,
           "and lands exactly where the 7.5 inch board has always had it");

        p.width = 960u;
        p.height = 680u;
        ok(!atc_panel_window(&p, 0x00080000u, &base, &end),
           "960x680 does not fit 512 KiB");
        ok(base == 0 && end == 0, "and a refused window reports nothing");
        ok(atc_panel_window(&p, 0x00200000u, &base, &end), "it fits 2 MiB");
        ok(end == 0x00200000u, "the window ends at the top of the chip");
        ok(base == 0x001D0000u, "and is three 64 KiB blocks long");
        ok(end - base - ATC_PANEL_MARKER >= 163200u, "with room for the picture");
        ok(base >= ATC_PANEL_LOW, "and never reaches into the reserved area");

        /* Anything in between works the same way. */
        ok(atc_panel_window(&p, 0x00100000u, &base, &end), "and 1 MiB too");
        ok(end == 0x00100000u && base == 0x000D0000u, "window at the top of 1 MiB");

        p.width = 880u;
        p.height = 528u;
        ok(atc_panel_window(&p, 0x00200000u, &base, &end), "the small glass on 2 MiB");
        ok(end == 0x00200000u && base == 0x001E0000u, "takes the minimum 128 KiB");

        /* A chip too small for anything at all. */
        ok(!atc_panel_window(&p, 0x00040000u, &base, &end), "256 KiB is too small");
        ok(!atc_panel_window(0, 0x00200000u, &base, &end), "null is refused");
    }

    /* The build-wide ceiling: a glass whose picture could not fit any of
     * these boards is refused wherever it runs.
     */
    p.width = 1024u;
    p.height = 496u;
    ok(atc_panel_usable(&p), "1024x496 is within the ceiling");
    p.width = 1024u;
    p.height = 6700u;
    ok(atc_panel_image_size(&p) > ATC_PANEL_IMAGE_MAX, "1024x6700 is past the ceiling");
    ok(!atc_panel_usable(&p), "and is refused");
    p.width = 880u;
    p.height = 528u;

    p.width = 880u;
    p.height = 528u;
    p.driver = 99u;
    ok(!atc_panel_usable(&p), "an unknown driver is refused");
    p.driver = ATC_DRV_ZKC75S;
    p.colors = 0u;
    ok(!atc_panel_usable(&p), "zero colours is refused");
    ok(!atc_panel_usable(0), "null is refused");
}

/* The advertisement an access point parses, and the CRC both sides have to
 * agree on. 0x313E is the CRC-16/CCITT-FALSE of eight zero bytes, which is
 * what a tag showing nothing transferred reports; the access point gets
 * there by running the same eight bytes through its own copy.
 */
static void test_adv_payload(void)
{
    uint8_t adv[31];
    uint8_t scan[31];
    uint8_t len;
    static const uint8_t name[10] = {'A', 'T', 'C', '_', '2', '6', '0', 'B', 'E', '2'};

    ok(oepl_crc16_ccitt((const uint8_t *)"", 0) == 0xFFFF, "an empty crc is the seed");
    ok(oepl_image_crc(0) == 0x313E, "no image is 0x313E");
    ok(oepl_image_crc(1) != 0x313E, "a real version is not");
    ok(oepl_image_crc(0x0123456789ABCDEFull) == oepl_image_crc(0x0123456789ABCDEFull),
       "the same version gives the same crc");
    {
        /* Little end first, which is the order the access point holds a
         * dataVer in. A version of 1 must not hash like 1 << 56.
         */
        ok(oepl_image_crc(1) != oepl_image_crc(0x0100000000000000ull),
           "the byte order is not symmetric");
    }

    ok(oepl_wake_byte(0, OEPL_WAKE_TIMED) == 0x00, "nothing to report");
    ok(oepl_wake_byte(1, OEPL_WAKE_BOOT) == 0x11, "one boot");
    ok(oepl_wake_byte(0x0F, OEPL_WAKE_WDT) == 0xFA, "counter and reason pack");
    ok(oepl_wake_byte(0x1F, 0xFF) == 0xFF, "both halves are masked");

    memset(adv, 0xAA, sizeof adv);
    len = oepl_adv_payload(adv, sizeof adv, 0x0074, 0x0036, 0x0342,
                           3250, 25, 7, 0x313E, 0x11);
    ok(len == 18, "the record is 18 bytes");
    ok(adv[0] == 17 && adv[1] == 0xFF, "a manufacturer record of 16 bytes");
    ok(adv[2] == 0x37 && adv[3] == 0x13, "company 0x1337");
    ok(adv[4] == 2, "layout version 2");
    ok(adv[5] == 0x74 && adv[6] == 0x00, "hardware type");
    ok(adv[7] == 0x36 && adv[8] == 0x00, "firmware version");
    ok(adv[9] == 0x42 && adv[10] == 0x03, "capabilities, extension bit set");
    ok((uint16_t)(adv[9] | (adv[10] << 8)) & OEPL_CAP_ADV_EXTENDED,
       "and an access point can see it");
    ok(adv[11] == (uint8_t)(3250u & 0xFF) && adv[12] == (uint8_t)(3250u >> 8), "battery");
    ok((int8_t)adv[13] == 25, "temperature");
    ok(adv[14] == 7, "counter");
    ok(adv[15] == 0x3E && adv[16] == 0x31, "image crc, little end first");
    ok(adv[17] == 0x11, "wake byte");
    ok(adv[18] == 0xAA, "and nothing past it");

    /* An access point that only knows version 2 reads the first 13 bytes of
     * manufacturer data and must find exactly what it did before.
     */
    ok(adv[4] == 2 && adv[14] == 7, "the v2 fields are where they were");

    ok(oepl_adv_payload(adv, 17, 0x0074, 1, 1, 1, 1, 1, 1, 1) == 0,
       "a short buffer is refused");
    ok(oepl_adv_payload(0, sizeof adv, 0x0074, 1, 1, 1, 1, 1, 1, 1) == 0,
       "and so is no buffer");

    memset(scan, 0xAA, sizeof scan);
    len = oepl_scan_payload(scan, sizeof scan, name, sizeof name);
    ok(len == 16, "the scan response is 16 bytes");
    ok(scan[0] == 3 && scan[1] == 0x03 && scan[2] == 0x37 && scan[3] == 0x13,
       "the service uuid");
    ok(scan[4] == 11 && scan[5] == 0x09, "a complete local name of ten");
    ok(memcmp(scan + 6, name, sizeof name) == 0, "the name itself");
    ok(scan[16] == 0xAA, "and nothing past it");
    ok(oepl_scan_payload(scan, 15, name, sizeof name) == 0, "a short buffer is refused");
    ok(oepl_scan_payload(scan, sizeof scan, name, 0) == 0, "an empty name is refused");

    /* Both together have to fit what the controller gives us: 31 bytes for
     * the advertisement, three of which the stack spends on the flags.
     */
    ok(18 + 3 <= 31, "the advertisement fits with the flags");
    ok(16 <= 31, "the scan response fits");
}

int main(void)
{
    test_pin_basics();
    test_pin_gate();
    test_pin_remove_and_max();
    test_pin_timeouts();
    test_protection_reply();
    test_log_stream();
    test_sysinfo_layout();
    test_settings_roundtrip();
    test_settings_blank();
    test_settings_address();
    test_panel_catalog();
    test_panel_limits();
    test_adv_payload();

    if (failures != 0) {
        printf("FAILED %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASS %d checks\n", checks);
    return 0;
}
