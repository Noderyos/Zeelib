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
inline int br_read(bitreader_t *br, size_t count);
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

#define min(a, b) ((a) < (b) ? (a) : (b))

inline int br_read(bitreader_t *br, size_t count) {
    assert(count < 32 && "Cannot read more than 32 bits");

    unsigned int out = 0;
    size_t processed = 0;

    while (processed < count) {
        size_t to_read = min(count - processed, 8 - br->_bit_offset);
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
#endif