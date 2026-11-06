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

void print_callback(uint8_t *buf, size_t len) {
    fwrite(buf, 1, len, stdout);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [zlib file]\n", *argv);
        return 1;
    }

    size_t zlib_len;
    uint8_t *zlib_data = read_file(argv[1], &zlib_len);

    int r = zeelib_decompress(zlib_data, zlib_len, print_callback);
    if (r < 0)
        return 1;

    free(zlib_data);
    return 0;
}
