#ifndef DITHER_TYPES_H
#define DITHER_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// Dithering kernel types
typedef enum {
    DITHER_FLOYD_STEINBERG = 0,
    DITHER_JARVIS,
    DITHER_STUCKI,
    DITHER_SIERRA_2_4A
} dither_kernel_t;

// Dithering configuration
typedef struct {
    dither_kernel_t kernel;
    bool serpentine;
    uint8_t palette[6][3];
} dither_config_t;

#endif
