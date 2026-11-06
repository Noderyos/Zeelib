#include <stdio.h>

#define ZEELIB_IMPLEMENTATION
#include <assert.h>
#include <stdlib.h>

#include "zeelib.h"

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

uint8_t *load_zlib(const char *filename, size_t *zlib_len, int *max_lz77_dist) {
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

    *max_lz77_dist = 1 << (cinfo+8);

    int fcheck = flg&0x1F;
    int fdict  = (flg>>5)&1;
    int flevel = (flg>>6)&3;

    zlib_data += fdict ? 6 : 2;

    fprintf(stderr, "[DEBUG] cm=%d cinfo=%d\n", cm, cinfo);
    fprintf(stderr,
        "[DEBUG] fcheck=%d fdict=%d flevel=%d\n", fcheck, fdict, flevel);

    *zlib_len = len - (fdict ? 6 : 2);
    return zlib_data;
}

void zlib_free(uint8_t *buf) {
    free(buf-2);
}

void print_callback(uint8_t *buf, size_t len) {
    fwrite(buf, 1, len, stdout);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [zlib file]\n", *argv);
        return 1;
    }

    size_t deflate_len;
    int max_lz77_dist;
    uint8_t *deflate_data = load_zlib(argv[1], &deflate_len, &max_lz77_dist);

    int r = zeelib_decompress(deflate_data, deflate_len, max_lz77_dist, print_callback);
    if (r < 0)
        return 1;


    zlib_free(deflate_data);
    return 0;
}
