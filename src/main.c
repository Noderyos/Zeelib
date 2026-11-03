#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BR_IMPLEMENTATION
#include "br.h"

uint8_t *read_file(const char *filename, size_t *len) {
    FILE *f = fopen(filename, "rb");
    assert(f != NULL && "File not found");

    fseek(f, 0, SEEK_END);
    size_t f_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(f_size);
    assert(fread(data, 1, f_size, f) == f_size && "WTF just happened ??");
    fclose(f);

    *len = f_size;
    return data;
}
uint8_t *load_zlib(const char *filename, size_t *zlib_len) {
    size_t len;
    uint8_t *zlib_data = read_file(filename, &len);
    fprintf(stderr, "[DEBUG] Loaded %s of size %ld\n", filename, len);

    assert(len > 2 && "What is this tiny file ??");
    uint8_t cmf = zlib_data[0];
    uint8_t flg = zlib_data[1];

    uint16_t crc = cmf<<8 | flg;
    assert(crc % 31 == 0 && "Invalid Checksum");

    int cm    =  cmf    &0xF;
    int cinfo = (cmf>>4)&0xF;

    assert(cm == 8 && "Unsupported zlib compression method");
    assert(cinfo < 8 && "Unsupported CINFO");

    int fcheck = flg&0x1F;
    int fdict  = (flg>>5)&1;
    int flevel = (flg>>6)&3;

    assert(fdict == 0 && "TODO support DICT");

    fprintf(stderr, "[DEBUG] cm=%d cinfo=%d\n", cm, cinfo);
    fprintf(stderr, "[DEBUG] fcheck=%d fdict=%d flevel=%d\n", fcheck, fdict, flevel);

    *zlib_len = len - 2;
    return zlib_data + 2;
}
void zlib_free(uint8_t *buf) {
    free(buf-2);
}

int inv_bits(int val, int count) {
    int out = 0;
    while (count--) {
        out = (out << 1) | (val&1);
        val >>= 1;
    }
    return out;
}

void inflate_raw(bitreader_t *br, uint8_t *out, size_t *out_len);
void inflate_fixed(bitreader_t *br, uint8_t* out, size_t* out_len);
void inflate_dynamic(bitreader_t* br, uint8_t* out, size_t* out_len);

void build_lut(const uint8_t *lens, int num_codes, uint16_t *out_codes);

int decode_symbol(bitreader_t *br, const uint8_t *lens, const uint16_t *codes, int num_codes);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [zlib file]\n", *argv);
        return 1;
    }

    size_t deflate_len;
    uint8_t *deflate_data = load_zlib(argv[1], &deflate_len);

    uint8_t *in = deflate_data;
    size_t in_len = deflate_len;
    uint8_t out[1024*1024];
    size_t out_len = 0;

    memset(out, 0, 1024*1024);

    bitreader_t br = br_init(in, in_len, 1);

    int quit = 0;
    while (!quit) {
        int bfinal = br_read(&br, 1);
        int btype = br_read(&br, 2);

        quit = bfinal;

        fprintf(stderr, "[DEBUG] bfinal=%d btype=%d\n", bfinal, btype);
        switch (btype) {
            case 0: inflate_raw(&br, out, &out_len); break;
            case 2: inflate_dynamic(&br, out, &out_len); break;
            case 1: inflate_fixed(&br, out, &out_len); break;
            case 3: assert(btype != 3 && "btype=3 is reserved");
            default: assert(0 && "unreachable");
        }
        fwrite(out, 1, out_len, stdout);
        out_len = 0;
    }

    zlib_free(deflate_data);
    return 0;
}

void inflate_raw(bitreader_t *br, uint8_t *out, size_t *out_len) {
    if (br->_bit_offset != 0) {
        br->_bit_offset = 0;
        br->_byte_offset++;
    }
    assert(br->_byte_offset + 4 < br->len && "Not enough data to decompress");

    uint16_t len = br->buffer[br->_byte_offset+1]<<8 | br->buffer[br->_byte_offset];
    uint16_t nlen = br->buffer[br->_byte_offset+3]<<8 | br->buffer[br->_byte_offset+2];
    br->_byte_offset += 4;

    assert(len == ((~nlen)&0xFFFF) && "Corrupted length");

    memcpy(&out[*out_len], &br->buffer[br->_byte_offset], len);
    *out_len += len;
    br->_byte_offset += len;
}

// Fixed Huffman

static const uint16_t LEN_BASE[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEN_EXTRA[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};

static const uint16_t DIST_BASE[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DIST_EXTRA[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void inflate_fixed(bitreader_t* br, uint8_t* out, size_t* out_len) {
    while (1) {
        int cp = inv_bits(br_read(br, 7), 7);
        int symbol = -1;
        if (cp < 24) { // 0000000-0010111 256-279 Length/Distance
            symbol = cp + 256;
        } else {
            cp = (cp << 1) | br_read(br, 1);
            if (cp >= 48 && cp < 192) { // 00110000-10111111 0-143 Literal
                symbol = cp - 48;
            } else if (cp >= 192 && cp < 200) { // 11000000-11000111 280-287 Length/Distance
                symbol = cp - 192 + 280;
            } else {
                cp = (cp << 1) | br_read(br, 1);
                if (cp >= 400 && cp < 512) { // 110010000-111111111 144-255 Literal
                    symbol = cp - 400 + 144;
                }
            }
        }
        assert(symbol >= 0 && symbol <= 285 && "Invalid Huffman symbol");

        if (symbol < 256) {
            out[(*out_len)++] = symbol;
        } else if (symbol == 256) {
            break;
        } else {
            int len_idx = symbol - 257;

            int length_bits = LEN_EXTRA[len_idx];
            uint32_t length = LEN_BASE[len_idx] + br_read(br, length_bits);

            uint32_t dist_code = inv_bits(br_read(br, 5), 5);
            assert(dist_code < 30 && "Invalid distance code");

            int dist_bits = DIST_EXTRA[dist_code];
            uint32_t distance = DIST_BASE[dist_code] + br_read(br, dist_bits);
            for (uint32_t i = 0; i < length; ++i) {
                out[*out_len] = out[*out_len - distance];
                (*out_len)++;
            }
        }
    }
}

// Dynamic Huffman
static const uint8_t CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

void inflate_dynamic(bitreader_t* br, uint8_t* out, size_t* out_len) {
    int hlit = br_read(br, 5) + 257;
    int hdist = br_read(br, 5) + 1;
    int hclen = br_read(br, 4) + 4;

    uint8_t clen_lengths[19] = {0};

    for (int i = 0; i < hclen; ++i) {
        clen_lengths[CLEN_ORDER[i]] = br_read(br, 3);
    }

    uint16_t clen_codes[19];
    build_lut(clen_lengths, 19, clen_codes);

    uint8_t lengths[286 + 32] = {0};
    int total_codes = hlit + hdist;
    int i = 0;

    while (i < total_codes) {
        int symbol = decode_symbol(br, clen_lengths, clen_codes, 19);

        if (symbol <= 15) {
            lengths[i++] = symbol;
        } else if (symbol == 16) {
            assert(i > 0 && "Cannot repeat without a prior length");

            uint8_t prev = lengths[i - 1];
            int repeat = br_read(br, 2) + 3;

            assert(i + repeat <= total_codes);
            while (repeat--)
                lengths[i++] = prev;
        } else if (symbol == 17) {
            int repeat = br_read(br, 3) + 3;

            assert(i + repeat <= total_codes);
            while (repeat--)
                lengths[i++] = 0;
        } else if (symbol == 18) {
            int repeat = br_read(br, 7) + 11;

            assert(i + repeat <= total_codes);
            while (repeat--)
                lengths[i++] = 0;
        }
    }
    uint8_t* lit_lens = &lengths[0];
    uint8_t* dist_lens = &lengths[hlit];

    uint16_t lit_codes[286];
    uint16_t dist_codes[32];

    build_lut(lit_lens, hlit, lit_codes);
    build_lut(dist_lens, hdist, dist_codes);

    while (1) {
        int symbol = decode_symbol(br, lit_lens, lit_codes, hlit);
        if (symbol < 256) {
            out[(*out_len)++] = symbol;
        } else if (symbol == 256) {
            break;
        } else {
            int len_idx = symbol - 257;

            int length_bits = LEN_EXTRA[len_idx];
            uint32_t length = LEN_BASE[len_idx] + br_read(br, length_bits);

            int dist_code = decode_symbol(br, dist_lens, dist_codes, hdist);
            assert(dist_code >= 0 && dist_code < 30 && "Invalid distance code");

            int dist_bits = DIST_EXTRA[dist_code];
            uint32_t distance = DIST_BASE[dist_code] + br_read(br, dist_bits);
            for (uint32_t i = 0; i < length; ++i) {
                out[*out_len] = out[*out_len - distance];
                (*out_len)++;
            }
        }
    }
}

void build_lut(const uint8_t *lens, int num_codes, uint16_t *out_codes) {
    int bl_counts[16] = {0};  // DEFLATE max bit length is 15
    int next_codes[16] = {0};
    int max_len = 0;

    for (int i = 0; i < num_codes; i++) {
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

    for (int i = 0; i < num_codes; i++) {
        int len = lens[i];
        if (len != 0) {
            out_codes[i] = next_codes[len];
            next_codes[len]++;
        }
    }
}

int decode_symbol(bitreader_t *br, const uint8_t *lens, const uint16_t *codes, int num_codes) {
    int code = 0;
    int code_bits = 0;

    int max_len = 0;
    for (int i = 0; i < num_codes; i++) {
        if (lens[i] > max_len) max_len = lens[i];
    }
    if (max_len == 0) return -1;

    while (code_bits < max_len) {
        code = (code << 1) | br_read(br, 1); // 1 bit MSB-first
        code_bits++;

        for (int j = 0; j < num_codes; j++) {
            if (lens[j] == code_bits && codes[j] == code) {
                return j;
            }
        }
    }
    assert(0 && "Invalid Huffman code in stream");
    return -1;
}