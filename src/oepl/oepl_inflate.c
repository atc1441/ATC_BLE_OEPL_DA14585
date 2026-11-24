#include "oepl_inflate.h"

#include <string.h>

#define WIN 4096
#define NEED 1
#define CONT 2
#define DONE 3
#define FAIL 4
#define PAUSE 5

typedef struct oepl_z {
    uint32_t acc;
    int bitcount;
    const uint8_t *in;
    uint16_t in_len;
    uint16_t in_pos;
    uint8_t window[WIN];
    uint16_t wpos;
    uint32_t s1;
    uint32_t s2;
    uint32_t out_count;
    uint8_t obuf[32];
    uint8_t ofill;
    uint8_t phase;
    uint8_t bfinal;
    uint8_t hdr_got;
    uint8_t hdr[2];
    uint8_t build_len;
    uint16_t build_code;
    uint16_t stored_left;
    uint16_t copy_len;
    uint16_t copy_dist;
    uint32_t hold;
    uint16_t hlit;
    uint16_t hdist;
    uint8_t hclen;
    uint8_t dyn_step;
    uint16_t dyn_pos;
    uint8_t prev_len;
    uint8_t clen[19];
    uint8_t litlen[288];
    uint8_t distlen[32];
    uint16_t lit_code[288];
    uint16_t dist_code[32];
    uint16_t clen_code[19];
    uint16_t lit_fast[512];
    uint16_t dist_fast[512];
    oepl_inflate_out out;
    void *user;
    int failed;
    int pause;
} oepl_z;

static oepl_z zstate;

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

void oepl_inflate_reset(void)
{
    memset(&zstate, 0, sizeof zstate);
    zstate.s1 = 1;
}

static int pull_bits(oepl_z *z, int n, unsigned *out)
{
    while (z->bitcount < n) {
        if (z->in_pos >= z->in_len) {
            return 0;
        }
        z->acc |= (uint32_t)z->in[z->in_pos++] << z->bitcount;
        z->bitcount += 8;
    }
    *out = (unsigned)(z->acc & ((1u << n) - 1u));
    z->acc >>= n;
    z->bitcount -= n;
    return 1;
}

static void drop_bits(oepl_z *z, int n)
{
    z->acc >>= n;
    z->bitcount -= n;
}

static int flush_out(oepl_z *z)
{
    int rc;

    if (z->ofill == 0) {
        return 1;
    }
    if (z->out != 0) {
        rc = z->out(z->user, z->obuf, z->ofill);
        /* Negative rejects the stream. Positive accepts these bytes and
         * asks the feeder to return so the caller can run the kernel.
         */
        if (rc < 0) {
            z->failed = 1;
            z->ofill = 0;
            return 0;
        }
        if (rc > 0) {
            z->pause = 1;
            z->ofill = 0;
            return 0;
        }
    }
    z->ofill = 0;
    return 1;
}

static int emit_byte(oepl_z *z, uint8_t value)
{
    z->window[z->wpos] = value;
    z->wpos = (uint16_t)((z->wpos + 1u) & (WIN - 1u));
    z->s1 += value;
    z->s2 += z->s1;
    if (z->s1 >= 65521u) {
        z->s1 -= 65521u;
    }
    if (z->s2 >= 65521u) {
        z->s2 %= 65521u;
    }
    z->out_count++;
    z->obuf[z->ofill++] = value;
    if (z->ofill == sizeof z->obuf) {
        return flush_out(z);
    }
    return 1;
}

static int assign_codes(const uint8_t *lens, int nsym, uint16_t *codes)
{
    int count[16];
    int next[16];
    int left;
    int len;
    int sym;
    unsigned code;

    memset(count, 0, sizeof count);
    memset(codes, 0, (size_t)nsym * sizeof codes[0]);
    for (sym = 0; sym < nsym; sym++) {
        if (lens[sym] > 15) {
            return 0;
        }
        count[lens[sym]]++;
    }
    count[0] = 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) {
            return 0;
        }
    }
    code = 0;
    next[0] = 0;
    for (len = 1; len <= 15; len++) {
        code = (code + (unsigned)count[len - 1]) << 1;
        next[len] = (int)code;
    }
    for (sym = 0; sym < nsym; sym++) {
        len = lens[sym];
        if (len != 0) {
            codes[sym] = (uint16_t)next[len]++;
        }
    }
    return 1;
}

static unsigned reverse_bits(unsigned value, int len)
{
    unsigned rev = 0;
    int i;

    for (i = 0; i < len; i++) {
        rev = (rev << 1) | ((value >> i) & 1u);
    }
    return rev;
}

static void fill_fast(const uint8_t *lens, const uint16_t *codes, int nsym, uint16_t *fast)
{
    int sym;
    int i;

    for (i = 0; i < 512; i++) {
        fast[i] = 0xFFFFu;
    }
    for (sym = 0; sym < nsym; sym++) {
        int len = lens[sym];
        unsigned key;
        unsigned mask;
        if (len == 0 || len > 9) {
            continue;
        }
        key = reverse_bits(codes[sym], len);
        mask = (1u << len) - 1u;
        for (i = 0; i < 512; i++) {
            if (((unsigned)i & mask) == key) {
                fast[i] = (uint16_t)((len << 9) | sym);
            }
        }
    }
}

static int decode_slow(oepl_z *z, const uint8_t *lens, const uint16_t *codes, int nsym, int *sym_out)
{
    while (z->build_len < 15) {
        unsigned bit;
        int sym;
        if (!pull_bits(z, 1, &bit)) {
            return NEED;
        }
        z->build_code = (uint16_t)((z->build_code << 1) | bit);
        z->build_len++;
        for (sym = 0; sym < nsym; sym++) {
            if (lens[sym] == z->build_len && codes[sym] == z->build_code) {
                *sym_out = sym;
                z->build_code = 0;
                z->build_len = 0;
                return CONT;
            }
        }
    }
    return FAIL;
}

static int decode_sym(oepl_z *z, const uint8_t *lens, const uint16_t *codes, const uint16_t *fast,
                      int nsym, int *sym_out)
{
    unsigned bits;
    uint16_t entry;

    if (z->build_len != 0) {
        return decode_slow(z, lens, codes, nsym, sym_out);
    }
    if (!pull_bits(z, 9, &bits)) {
        /* Put nothing back: pull_bits only consumes when it succeeds. */
        return decode_slow(z, lens, codes, nsym, sym_out);
    }
    entry = fast[bits & 511u];
    if (entry != 0xFFFFu) {
        int len = entry >> 9;
        int extra = 9 - len;
        /* pull_bits already removed 9 bits. The unused tail is the next
         * stream bits and has to go back onto the low end of the accumulator.
         */
        if (extra > 0) {
            z->acc = (z->acc << extra) | ((bits >> len) & ((1u << extra) - 1u));
            z->bitcount += extra;
        }
        *sym_out = (int)(entry & 511);
        return CONT;
    }
    /* The 9 peeked bits were consumed. Rebuild them as a prefix and continue slowly. */
    z->build_code = (uint16_t)reverse_bits(bits & 511u, 9);
    z->build_len = 9;
    return decode_slow(z, lens, codes, nsym, sym_out);
}

static int set_fixed(oepl_z *z)
{
    int i;

    for (i = 0; i <= 143; i++) {
        z->litlen[i] = 8;
    }
    for (i = 144; i <= 255; i++) {
        z->litlen[i] = 9;
    }
    for (i = 256; i <= 279; i++) {
        z->litlen[i] = 7;
    }
    for (i = 280; i <= 287; i++) {
        z->litlen[i] = 8;
    }
    for (i = 0; i < 32; i++) {
        z->distlen[i] = 5;
    }
    if (!assign_codes(z->litlen, 288, z->lit_code) || !assign_codes(z->distlen, 32, z->dist_code)) {
        return 0;
    }
    fill_fast(z->litlen, z->lit_code, 288, z->lit_fast);
    fill_fast(z->distlen, z->dist_code, 32, z->dist_fast);
    return 1;
}

static int copy_one(oepl_z *z)
{
    uint16_t from;
    if (z->copy_dist == 0 || z->copy_dist > z->out_count || z->copy_dist > WIN) {
        return 0;
    }
    from = (uint16_t)((z->wpos - z->copy_dist) & (WIN - 1u));
    return emit_byte(z, z->window[from]);
}

static int step(oepl_z *z)
{
    unsigned bits;
    int sym;
    int rc;

    switch (z->phase) {
    case 0: /* zlib header */
        while (z->hdr_got < 2) {
            if (z->in_pos >= z->in_len) {
                return NEED;
            }
            z->hdr[z->hdr_got++] = z->in[z->in_pos++];
        }
        if ((z->hdr[0] & 0x0Fu) != 8u) {
            return FAIL;
        }
        if ((((unsigned)z->hdr[0] << 8) + z->hdr[1]) % 31u != 0u) {
            return FAIL;
        }
        if (z->hdr[1] & 0x20u) {
            return FAIL; /* preset dictionary */
        }
        if ((z->hdr[0] >> 4) > 4) {
            return FAIL; /* window above 4096 */
        }
        z->phase = 1;
        return CONT;

    case 1: /* block header */
        if (!pull_bits(z, 3, &bits)) {
            return NEED;
        }
        z->bfinal = (uint8_t)(bits & 1u);
        bits >>= 1;
        if (bits == 0) {
            int extra = z->bitcount & 7;
            if (extra) {
                drop_bits(z, extra);
            }
            z->phase = 2;
            return CONT;
        }
        if (bits == 1) {
            if (!set_fixed(z)) {
                return FAIL;
            }
            z->phase = 6;
            return CONT;
        }
        if (bits == 2) {
            z->dyn_step = 0;
            z->phase = 4;
            return CONT;
        }
        return FAIL;

    case 2: /* stored length */
        if (!pull_bits(z, 16, &bits)) {
            return NEED;
        }
        z->stored_left = (uint16_t)bits;
        z->phase = 3;
        return CONT;

    case 3: { /* stored nlen, then bytes */
        unsigned nlen;
        if (z->dyn_step == 0) {
            if (!pull_bits(z, 16, &nlen)) {
                return NEED;
            }
            if ((uint16_t)(~z->stored_left) != (uint16_t)nlen) {
                return FAIL;
            }
            z->dyn_step = 1;
        }
        while (z->stored_left != 0) {
            if (z->in_pos >= z->in_len) {
                return NEED;
            }
            if (!emit_byte(z, z->in[z->in_pos++])) {
                if (z->pause) {
                    z->stored_left--;
                    return PAUSE;
                }
                return FAIL;
            }
            z->stored_left--;
        }
        z->dyn_step = 0;
        z->dyn_pos = 0;
        z->hold = 0;
        z->phase = z->bfinal ? 8 : 1;
        return CONT;
    }

    case 4: /* dynamic header sizes */
        if (!pull_bits(z, 14, &bits)) {
            return NEED;
        }
        z->hlit = (uint16_t)((bits & 31u) + 257u);
        z->hdist = (uint16_t)(((bits >> 5) & 31u) + 1u);
        z->hclen = (uint8_t)(((bits >> 10) & 15u) + 4u);
        if (z->hlit > 286 || z->hdist > 32) {
            return FAIL;
        }
        memset(z->clen, 0, sizeof z->clen);
        z->dyn_pos = 0;
        z->phase = 5;
        return CONT;

    case 5: /* code-length codes, then lit/dist lengths */
        while (z->dyn_pos < z->hclen) {
            if (!pull_bits(z, 3, &bits)) {
                return NEED;
            }
            z->clen[clen_order[z->dyn_pos]] = (uint8_t)bits;
            z->dyn_pos++;
        }
        if (!assign_codes(z->clen, 19, z->clen_code)) {
            return FAIL;
        }
        memset(z->litlen, 0, sizeof z->litlen);
        memset(z->distlen, 0, sizeof z->distlen);
        z->dyn_pos = 0;
        z->prev_len = 0;
        z->phase = 7;
        return CONT;

    case 7: /* fill litlen then dist lengths */
        while (z->dyn_pos < (uint16_t)(z->hlit + z->hdist)) {
            uint8_t *dest;
            int nsym_here;
            if (z->dyn_step == 0) {
                rc = decode_slow(z, z->clen, z->clen_code, 19, &sym);
                if (rc != CONT) {
                    return rc;
                }
                if (sym < 16) {
                    z->prev_len = (uint8_t)sym;
                    z->copy_len = 1;
                    z->dyn_step = 2;
                } else if (sym == 16) {
                    z->phase = 7;
                    z->dyn_step = 1;
                    z->copy_dist = 2; /* extra bits */
                    z->copy_len = 3;
                } else if (sym == 17) {
                    z->prev_len = 0;
                    z->dyn_step = 1;
                    z->copy_dist = 3;
                    z->copy_len = 3;
                } else if (sym == 18) {
                    z->prev_len = 0;
                    z->dyn_step = 1;
                    z->copy_dist = 7;
                    z->copy_len = 11;
                } else {
                    return FAIL;
                }
            }
            if (z->dyn_step == 1) {
                if (!pull_bits(z, (int)z->copy_dist, &bits)) {
                    return NEED;
                }
                z->copy_len = (uint16_t)(z->copy_len + bits);
                z->dyn_step = 2;
            }
            dest = z->litlen;
            nsym_here = (int)z->hlit;
            if (z->dyn_pos >= z->hlit) {
                dest = z->distlen;
                nsym_here = (int)z->hdist;
            }
            while (z->copy_len != 0) {
                int index = (int)z->dyn_pos;
                if (z->dyn_pos >= z->hlit) {
                    index = (int)(z->dyn_pos - z->hlit);
                    dest = z->distlen;
                    nsym_here = (int)z->hdist;
                } else {
                    dest = z->litlen;
                    nsym_here = (int)z->hlit;
                }
                if (index >= nsym_here) {
                    return FAIL;
                }
                dest[index] = z->prev_len;
                z->dyn_pos++;
                z->copy_len--;
            }
            z->dyn_step = 0;
        }
        if (!assign_codes(z->litlen, z->hlit, z->lit_code)
            || !assign_codes(z->distlen, z->hdist, z->dist_code)) {
            return FAIL;
        }
        fill_fast(z->litlen, z->lit_code, z->hlit, z->lit_fast);
        fill_fast(z->distlen, z->dist_code, z->hdist, z->dist_fast);
        z->phase = 6;
        return CONT;

    case 6: /* literal / length symbols */
        rc = decode_sym(z, z->litlen, z->lit_code, z->lit_fast, z->hlit ? z->hlit : 288, &sym);
        if (rc != CONT) {
            return rc;
        }
        if (sym < 256) {
            if (!emit_byte(z, (uint8_t)sym)) {
                return z->pause ? PAUSE : FAIL;
            }
            return CONT;
        }
        if (sym == 256) {
            z->dyn_step = 0;
            z->dyn_pos = 0;
            z->hold = 0;
            z->phase = z->bfinal ? 8 : 1;
            return CONT;
        }
        if (sym > 285) {
            return FAIL;
        }
        z->copy_len = len_base[sym - 257];
        z->dyn_step = len_extra[sym - 257];
        z->phase = 9;
        return CONT;

    case 9: /* length extra, then distance */
        if (z->dyn_step != 0) {
            if (!pull_bits(z, z->dyn_step, &bits)) {
                return NEED;
            }
            z->copy_len = (uint16_t)(z->copy_len + bits);
        }
        z->phase = 10;
        z->build_len = 0;
        z->build_code = 0;
        return CONT;

    case 10:
        rc = decode_sym(z, z->distlen, z->dist_code, z->dist_fast, z->hdist ? z->hdist : 32, &sym);
        if (rc != CONT) {
            return rc;
        }
        if (sym > 29) {
            return FAIL;
        }
        z->copy_dist = dist_base[sym];
        z->dyn_step = dist_extra[sym];
        z->phase = 11;
        return CONT;

    case 11:
        if (z->dyn_step != 0) {
            if (!pull_bits(z, z->dyn_step, &bits)) {
                return NEED;
            }
            z->copy_dist = (uint16_t)(z->copy_dist + bits);
        }
        z->phase = 12;
        return CONT;

    case 12:
        while (z->copy_len != 0) {
            if (!copy_one(z)) {
                if (z->pause) {
                    z->copy_len--;
                    return PAUSE;
                }
                return FAIL;
            }
            z->copy_len--;
            if (z->failed) {
                return FAIL;
            }
        }
        z->phase = 6;
        return CONT;

    case 8: { /* Adler32 */
        unsigned byte;
        uint32_t got;
        int i;
        if (z->dyn_step == 0) {
            int extra = z->bitcount & 7;
            if (extra) {
                drop_bits(z, extra);
            }
            z->dyn_pos = 0;
            z->dyn_step = 1;
            z->hold = 0;
        }
        while (z->dyn_pos < 4) {
            if (z->bitcount >= 8) {
                byte = (unsigned)(z->acc & 0xFFu);
                drop_bits(z, 8);
            } else if (z->in_pos < z->in_len) {
                byte = z->in[z->in_pos++];
            } else {
                return NEED;
            }
            z->hold = (z->hold << 8) | byte;
            z->dyn_pos++;
        }
        got = (z->s2 << 16) | z->s1;
        if (got != z->hold) {
            return FAIL;
        }
        (void)i;
        z->phase = 13;
        return DONE;
    }

    case 13:
        return DONE;

    default:
        return FAIL;
    }
}

int oepl_inflate_feed(const uint8_t *data, uint16_t len, uint16_t *used,
                      oepl_inflate_out out, void *user)
{
    oepl_z *z = &zstate;
    int rc;

    if (used == 0) {
        return -1;
    }
    z->in = data;
    z->in_len = len;
    z->in_pos = 0;
    z->out = out;
    z->user = user;
    z->pause = 0;
    if (z->phase == 13) {
        *used = 0;
        return -1;
    }
    for (;;) {
        rc = step(z);
        if (rc == CONT) {
            continue;
        }
        if (rc == PAUSE) {
            *used = z->in_pos;
            return 2;
        }
        if (!flush_out(z)) {
            *used = z->in_pos;
            if (z->pause && !z->failed) {
                /* The member is already finished. Pausing here would make
                 * the next feed see a completed stream and reject it.
                 */
                return (rc == DONE) ? 1 : 2;
            }
            return -1;
        }
        *used = z->in_pos;
        if (rc == NEED) {
            return 0;
        }
        if (rc == DONE) {
            return 1;
        }
        return -1;
    }
}
