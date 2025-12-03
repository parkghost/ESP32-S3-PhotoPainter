#ifndef DITHER_ENGINE_H
#define DITHER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "dither_types.h"

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;      // File type must be "BM"
    uint32_t bfSize;      // File size
    uint16_t bfReserved1; // Reserved
    uint16_t bfReserved2; // Reserved
    uint32_t bfOffBits;   // Pixel data offset
} BITMAPFILEHEADER;

typedef struct {
    uint32_t biSize;          // Size of this structure
    int32_t  biWidth;         // Image width
    int32_t  biHeight;        // Image height (positive value indicates reverse storage)
    uint16_t biPlanes;        // Must be 1
    uint16_t biBitCount;      // Bit depth per pixel, here it is 24
    uint32_t biCompression;   // Compression method, 0 = BI_RGB
    uint32_t biSizeImage;     // Image data size (can be 0)
    int32_t  biXPelsPerMeter; // Horizontal resolution
    int32_t  biYPelsPerMeter; // Vertical resolution
    uint32_t biClrUsed;       // Number of used color indices
    uint32_t biClrImportant;  // Number of important colors
} BITMAPINFOHEADER;
#pragma pack(pop)

class dither_engine {
private:
    dither_config_t _config;
    int nearest_color_perceptual(uint8_t r, uint8_t g, uint8_t b);

public:
    dither_engine();
    ~dither_engine();

    // Load config from settings (call once at startup)
    void set_config(const dither_config_t *config);

    // JPEG decode - Decode RGB888 data, *outbuffer: No memory allocation required
    uint8_t Jpeg_decode(uint8_t *inbuffer, int inlen, uint8_t **outbuffer, int *outlen, int *width, int *height);

    // Remember to release the outbuffer when the usage is completed
    void Jpeg_dec_buffer_free(uint8_t *outbuffer);

    // Main dithering method (uses internal config)
    void dither_rgb888(uint8_t *in_img, uint8_t *out_img, int w, int h);

    // Convert RGB888 to BMP and save to SD card
    int rgb888_to_sdcard_bmp(const char *filename, const uint8_t *rgb888, int width, int height);
};

#endif
