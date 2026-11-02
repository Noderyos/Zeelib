#ifndef BR_H
#define BR_H

#include <stdint.h>

typedef struct bitreader {
    uint8_t *buffer;
    size_t len;
    int lsb;
    size_t _byte_offset;
    size_t _bit_offset;
} bitreader_t;

bitreader_t br_init(uint8_t *buffer, size_t len, int lsb);
int br_read(bitreader_t *br, int count);
#endif

#ifdef BR_IMPLEMENTATION
#undef BR_IMPLEMENTATION
bitreader_t br_init(uint8_t *buffer, size_t len, int lsb) {
    return (bitreader_t){
        .buffer = buffer,
        .len = len,
        .lsb = lsb,
        ._byte_offset = 0,
        ._bit_offset = 0
    };
}

int br_read(bitreader_t *br, int count) {
    assert(count >= 0 && count < 32 && "Cannot read more than 32 bits");

    int out = 0;
    for (int i = 0; i < count; ++i) {
        size_t bit_idx = br->_bit_offset;
        if (!br->lsb) bit_idx = 7 - bit_idx;

        int bit = (br->buffer[br->_byte_offset] >> bit_idx) & 1;
        out |= bit << i;

        br->_bit_offset++;
        if (br->_bit_offset == 8) {
            br->_bit_offset = 0;
            br->_byte_offset++;
        }
    }

    return out;
}
#endif