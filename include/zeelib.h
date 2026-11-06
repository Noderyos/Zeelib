#ifndef ZEELIB_H
#define ZEELIB_H

#include <stdint.h>

int zeelib_decompress(
    uint8_t *in, size_t in_len,
    void (*cb)(uint8_t *, size_t)
);

int zeelib_raw_decompress(
    uint8_t *in, size_t in_len, int max_lz77_dist,
    void (*cb)(uint8_t *, size_t)
);

#endif

#ifdef ZEELIB_IMPLEMENTATION
#undef ZEELIB_IMPLEMENTATION

#include <string.h>

#define OUT_SIZE 0x10000 // max size of inflate raw / 2x max LZ77 window

#ifdef ZEELIB_ASSERT
#define _ZL_ASSERT(x, reason) \
    if (!(x)) {fprintf(stderr, "[ERROR] " reason "\n");return -1;}
#else
#define _ZL_ASSERT(x, reason) if (!(x)) return -1;
#endif

typedef struct _zl_br {
    uint8_t *buffer;
    size_t len;
    int lsb;
    size_t _byte_offset;
    size_t _bit_offset;
} _zl_br_t;

_zl_br_t _zl_br_init(uint8_t *buffer, size_t len, int lsb) {
    return (_zl_br_t){
        .buffer = buffer,
        .len = len,
        .lsb = lsb,
        ._byte_offset = 0,
        ._bit_offset = 0
    };
}

#define _lz_min(a, b) ((a) < (b) ? (a) : (b))

int _zl_br_read(_zl_br_t *br, size_t count) {
    assert(count < 32 && "Cannot read more than 32 bits");

    unsigned int out = 0;
    size_t processed = 0;

    while (processed < count) {
        size_t to_read = _lz_min(count - processed, 8 - br->_bit_offset);
        unsigned int mask = (1U << to_read) - 1;
        unsigned int chunk;

        if (br->lsb) chunk = (br->buffer[br->_byte_offset] >> br->_bit_offset) & mask;
        else chunk = (br->buffer[br->_byte_offset] >> (8 - br->_bit_offset - to_read)) & mask;

        out |= chunk << processed;

        processed += to_read;
        br->_bit_offset += to_read;

        if (br->_bit_offset == 8) {
            br->_bit_offset = 0;
            br->_byte_offset++;
        }
    }

    return (int)out;
}

int zeelib_decompress(
    uint8_t *in, size_t in_len,
    void (*cb)(uint8_t *, size_t)
) {
    _ZL_ASSERT(in_len > 2, "Not enough data for zlib header");

    uint8_t cmf = in[0];
    uint8_t flg = in[1];

    uint16_t crc = cmf<<8 | flg;
    _ZL_ASSERT(crc % 31 == 0, "Invalid zlib checksum");

    int cm    =  cmf    &0xF;
    int cinfo = (cmf>>4)&0xF;

    _ZL_ASSERT(cm == 8, "Unsupported zlib compression method");
    _ZL_ASSERT(cinfo < 8, "Unsupported zlib CINFO");

    int max_lz77_dist = 1 << (cinfo+8);

    int fcheck = flg&0x1F;
    int fdict  = (flg>>5)&1;
    int flevel = (flg>>6)&3;
    (void)fcheck;
    (void)flevel;

    uint8_t *zlib_in = in + (fdict ? 6 : 2);
    size_t zlib_len = in_len - (fdict ? 6 : 2);
    return zeelib_raw_decompress(zlib_in, zlib_len, max_lz77_dist, cb);
}

void _zl_flush_out(
    uint8_t out[OUT_SIZE], size_t *out_len, size_t max_lz77_dist,
    void (*cb)(uint8_t *, size_t)
) {
    size_t to_flush = *out_len - max_lz77_dist;
    cb(out, to_flush);
    memmove(out, &out[to_flush], *out_len - to_flush);
    *out_len = max_lz77_dist;
}

int _zl_inflate_raw(_zl_br_t *br, uint8_t out[OUT_SIZE], size_t *out_len) {
    if (br->_bit_offset != 0) {
        br->_bit_offset = 0;
        br->_byte_offset++;
    }

    _ZL_ASSERT(br->_byte_offset + 4 < br->len, "Not enough data to decompress");

    size_t bo = br->_byte_offset;
    uint16_t len = br->buffer[bo+1]<<8 | br->buffer[bo];
    uint16_t nlen = br->buffer[bo+3]<<8 | br->buffer[bo+2];
    br->_byte_offset += 4;

    _ZL_ASSERT(len == ((~nlen)&0xFFFF), "Corrupted length");

    memcpy(&out[*out_len], &br->buffer[br->_byte_offset], len);
    *out_len += len;
    br->_byte_offset += len;
    return 0;
}

// Fixed Huffman

static const uint16_t _ZL_LEN_BASE[] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t _ZL_LEN_EXTRA[] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};

static const uint16_t _ZL_DIST_BASE[] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,
    769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t _ZL_DIST_EXTRA[] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

int _zl_inv_bits(int val, int count) {
    int out = 0;
    while (count--) {out = (out << 1)|(val&1); val >>= 1;}
    return out;
}

int _zl_inflate_fixed(
    _zl_br_t* br,
    uint8_t out[OUT_SIZE], size_t* out_len, int max_lz77_dist,
    void (*cb)(uint8_t *, size_t)
) {
    while (1) {
        int cp = _zl_inv_bits(_zl_br_read(br, 7), 7);
        int symbol = -1;
        if (cp < 24) { // 0000000-0010111 256-279 Length/Distance
            symbol = cp + 256;
        } else {
            cp = (cp << 1) | _zl_br_read(br, 1);
            if (cp >= 48 && cp < 192) { // 00110000-10111111 0-143 Literal
                symbol = cp - 48;
            } else if (cp >= 192 && cp < 200) { // 11000000-11000111 280-287 Length/Distance
                symbol = cp - 192 + 280;
            } else {
                cp = (cp << 1) | _zl_br_read(br, 1);
                if (cp >= 400 && cp < 512) { // 110010000-111111111 144-255 Literal
                    symbol = cp - 400 + 144;
                }
            }
        }
        _ZL_ASSERT(symbol >= 0 && symbol <= 285, "Invalid Huffman symbol");

        if (symbol < 256) {
            if (*out_len + 1 >= OUT_SIZE)
                _zl_flush_out(out, out_len, max_lz77_dist, cb);

            out[(*out_len)++] = symbol;
        } else if (symbol == 256) {
            break;
        } else {
            int len_idx = symbol - 257;

            int length_bits = _ZL_LEN_EXTRA[len_idx];
            uint32_t length = _ZL_LEN_BASE[len_idx] + _zl_br_read(br, length_bits);

            uint32_t dist_code = _zl_inv_bits(_zl_br_read(br, 5), 5);
            _ZL_ASSERT(dist_code < 30, "Invalid distance code");

            int dist_bits = _ZL_DIST_EXTRA[dist_code];
            uint32_t distance = _ZL_DIST_BASE[dist_code] + _zl_br_read(br, dist_bits);

            if (*out_len + length >= OUT_SIZE)
                _zl_flush_out(out, out_len, max_lz77_dist, cb);

            uint8_t *dst = &out[*out_len];
            uint8_t *src = dst - distance;
            uint8_t *end = dst + length;
            while (dst != end) {
                *dst++ = *src++;
            }
            *out_len += length;
        }
    }
    return 0;
}

// Dynamic Huffman

typedef struct _zl_lut_entry {
    uint8_t len;
    uint16_t code;
} _zl_lut_entry_t;

static const uint8_t _ZL_CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

void _zl_build_codes(const uint8_t *lens, size_t num_codes, uint16_t *out_codes) {
    int bl_counts[16] = {0};  // DEFLATE max bit length is 15
    int next_codes[16] = {0};
    int max_len = 0;

    for (size_t i = 0; i < num_codes; i++) {
        out_codes[i] = 0;
        if (lens[i] > 0) {
            bl_counts[lens[i]]++;
            if (lens[i] > max_len) max_len = lens[i];
        }
    }

    int code = 0;
    bl_counts[0] = 0;
    for (int bits = 1; bits <= max_len; bits++) {
        code = (code + bl_counts[bits - 1]) << 1;
        next_codes[bits] = code;
    }

    for (size_t i = 0; i < num_codes; i++) {
        int len = lens[i];
        if (len != 0) {
            out_codes[i] = next_codes[len];
            next_codes[len]++;
        }
    }
}

void _zl_build_lut(
    uint16_t *codes, uint8_t *lens, size_t num_codes,
    _zl_lut_entry_t *out_lut, size_t lut_size
) {
    for (size_t i = 0; i < lut_size; ++i) {
        out_lut[i].len = 0;
    }

    for (size_t i = 0; i < num_codes; ++i) {
        if (lens[i] > 0) {
            out_lut[codes[i]].len = lens[i];
            out_lut[codes[i]].code = i;
        }
    }
}

int _zl_decode_symbol(_zl_br_t *br, _zl_lut_entry_t *lut) {
    int code = 0;
    int code_bits = 0;

    while (code_bits < 16) {
        code = (code << 1) | _zl_br_read(br, 1); // 1 bit MSB-first
        code_bits++;

        if (lut[code].len == code_bits)
            return lut[code].code;
    }
    _ZL_ASSERT(0, "Invalid Huffman code in stream");
    return -1;
}

int _zl_inflate_dynamic(
    _zl_br_t* br,
    uint8_t out[OUT_SIZE], size_t* out_len, int max_lz77_dist,
    void (*cb)(uint8_t *, size_t)
) {
    int hlit = _zl_br_read(br, 5) + 257;
    int hdist = _zl_br_read(br, 5) + 1;
    int hclen = _zl_br_read(br, 4) + 4;

    uint8_t clen_lengths[19] = {0};

    for (int i = 0; i < hclen; ++i) {
        clen_lengths[_ZL_CLEN_ORDER[i]] = _zl_br_read(br, 3);
    }

    uint16_t clen_codes[19];
    _zl_build_codes(clen_lengths, 19, clen_codes);

    _zl_lut_entry_t clen_lut[128]; // lengths are picked from 3 bits => 7bits
    _zl_build_lut(clen_codes, clen_lengths, 19, clen_lut, 128);

    uint8_t lengths[286 + 32] = {0};
    int total_codes = hlit + hdist;
    int i = 0;

    while (i < total_codes) {
        int symbol = _zl_decode_symbol(br, clen_lut);

        if (symbol <= 15) {
            lengths[i++] = symbol;
        } else if (symbol == 16) {
            _ZL_ASSERT(i > 0, "Cannot repeat without a prior length");

            uint8_t prev = lengths[i - 1];
            int repeat = _zl_br_read(br, 2) + 3;

            _ZL_ASSERT(i + repeat <= total_codes, "Too much codes");
            while (repeat--)
                lengths[i++] = prev;
        } else if (symbol == 17) {
            int repeat = _zl_br_read(br, 3) + 3;

            _ZL_ASSERT(i + repeat <= total_codes, "Too much codes");
            while (repeat--)
                lengths[i++] = 0;
        } else if (symbol == 18) {
            int repeat = _zl_br_read(br, 7) + 11;

            _ZL_ASSERT(i + repeat <= total_codes, "Too much codes");
            while (repeat--)
                lengths[i++] = 0;
        }
    }
    uint8_t* lit_lens = &lengths[0];
    uint8_t* dist_lens = &lengths[hlit];

    uint16_t lit_codes[286];
    uint16_t dist_codes[32];

    _zl_build_codes(lit_lens, hlit, lit_codes);
    _zl_build_codes(dist_lens, hdist, dist_codes);

    _zl_lut_entry_t lit_lut[32768]; // max length is 15 bits
    _zl_build_lut(lit_codes, lit_lens, hlit, lit_lut, 32768);
    _zl_lut_entry_t dist_lut[32768];
    _zl_build_lut(dist_codes, dist_lens, hdist, dist_lut, 32768);

    while (1) {
        int symbol = _zl_decode_symbol(br, lit_lut);
        if (symbol < 256) {
            if (*out_len + 1 >= OUT_SIZE)
                _zl_flush_out(out, out_len, max_lz77_dist, cb);

            out[(*out_len)++] = symbol;
        } else if (symbol == 256) {
            break;
        } else {
            int len_idx = symbol - 257;

            int length_bits = _ZL_LEN_EXTRA[len_idx];
            uint32_t length = _ZL_LEN_BASE[len_idx] + _zl_br_read(br, length_bits);

            int dist_code = _zl_decode_symbol(br, dist_lut);
            _ZL_ASSERT(dist_code >= 0 && dist_code < 30, "Invalid distance code");

            int dist_bits = _ZL_DIST_EXTRA[dist_code];
            uint32_t distance = _ZL_DIST_BASE[dist_code] + _zl_br_read(br, dist_bits);

            if (*out_len + length >= OUT_SIZE)
                _zl_flush_out(out, out_len, max_lz77_dist, cb);

            uint8_t *dst = &out[*out_len];
            uint8_t *src = dst - distance;
            uint8_t *end = dst + length;
            while (dst != end) {
                *dst++ = *src++;
            }
            *out_len += length;
        }
    }
    return 0;
}

int zeelib_raw_decompress(
    uint8_t *in, size_t in_len, int max_lz77_dist,
    void (*cb)(uint8_t *, size_t)
) {
    uint8_t out[OUT_SIZE];
    memset(out, 0, OUT_SIZE);
    size_t out_len = 0;

    _zl_br_t br = _zl_br_init(in, in_len, 1);

    int quit = 0;
    while (!quit) {
        int bfinal = _zl_br_read(&br, 1);
        int btype = _zl_br_read(&br, 2);

        quit = bfinal;

        switch (btype) {
            case 0:
                if (_zl_inflate_raw(&br, out, &out_len) < 0)
                    return -1;
                break;
            case 2:
                if (_zl_inflate_dynamic(&br, out, &out_len, max_lz77_dist, cb) < 0)
                    return -1;
                break;
            case 1:
                if (_zl_inflate_fixed(&br, out, &out_len, max_lz77_dist, cb) < 0)
                    return -1;
                break;
            case 3: _ZL_ASSERT(0, "btype=3 is reserved");
            default: _ZL_ASSERT(0, "unreachable");
        }

        cb(out, out_len);
        out_len = 0;
    }
    return 0;
}

#endif
