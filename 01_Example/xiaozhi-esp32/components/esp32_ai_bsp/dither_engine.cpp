#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "dither_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdcard_bsp.h"

#include "jpeg_decoder.h"
#include "test_decoder.h"

static const char *TAG = "dither";

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

// Default 6-color palette for Waveshare 7.3" e-paper display
static const uint8_t DEFAULT_PALETTE[6][3] = {
    {0, 0, 0},       // Black
    {255, 255, 255}, // White
    {255, 0, 0},     // Red
    {0, 255, 0},     // Green
    {0, 0, 255},     // Blue
    {255, 255, 0}    // Yellow
};

// ============================================================================
// Error Diffusion Kernels
// ============================================================================

// Jarvis-Judice-Ninke kernel: 48 divisor, 3 rows (best quality)
//         *   7   5
//     3   5   7   5   3
//     1   3   5   3   1
static const int JARVIS_WEIGHTS[12] = {7, 5, 3, 5, 7, 5, 3, 1, 3, 5, 3, 1};
static const int JARVIS_OFFSETS_X[12] = {1, 2, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2};
static const int JARVIS_OFFSETS_Y[12] = {0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2};
#define JARVIS_DIVISOR 48
#define JARVIS_COUNT 12

// Stucki kernel: 42 divisor (similar to Jarvis but slightly different weights)
//         *   8   4
//     2   4   8   4   2
//     1   2   4   2   1
static const int STUCKI_WEIGHTS[12] = {8, 4, 2, 4, 8, 4, 2, 1, 2, 4, 2, 1};
// Same offsets as Jarvis
#define STUCKI_DIVISOR 42
#define STUCKI_COUNT 12

// Sierra-2-4A kernel: 4 divisor (fast, simple)
//     *   2
//     1   1
static const int SIERRA_WEIGHTS[3] = {2, 1, 1};
static const int SIERRA_OFFSETS_X[3] = {1, -1, 0};
static const int SIERRA_OFFSETS_Y[3] = {0, 1, 1};
#define SIERRA_DIVISOR 4
#define SIERRA_COUNT 3

// Floyd-Steinberg kernel: 16 divisor (classic)
//     *   7
// 3   5   1
static const int FS_WEIGHTS[4] = {7, 3, 5, 1};
static const int FS_OFFSETS_X[4] = {1, -1, 0, 1};
static const int FS_OFFSETS_Y[4] = {0, 1, 1, 1};
#define FS_DIVISOR 16
#define FS_COUNT 4

// ============================================================================
// Perceptual Color Distance (Redmean formula)
// ============================================================================
// This formula accounts for human perception where we're more sensitive to green
// and the perception of red changes based on the amount of red present.
static inline int perceptual_distance(uint8_t r1, uint8_t g1, uint8_t b1,
                                       uint8_t r2, uint8_t g2, uint8_t b2) {
    int dr = (int)r1 - r2;
    int dg = (int)g1 - g2;
    int db = (int)b1 - b2;
    int rmean = ((int)r1 + r2) / 2;
    // Weighted color distance based on human perception
    return ((512 + rmean) * dr * dr >> 8) + 4 * dg * dg + ((767 - rmean) * db * db >> 8);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

dither_engine::dither_engine() {
    // Initialize with defaults (best quality settings)
    _config.kernel = DITHER_JARVIS;
    _config.serpentine = true;
    memcpy(_config.palette, DEFAULT_PALETTE, sizeof(DEFAULT_PALETTE));
    ESP_LOGI(TAG, "Dither engine initialized with Jarvis kernel + serpentine scanning");
}

dither_engine::~dither_engine() {
}

void dither_engine::set_config(const dither_config_t *config) {
    if (config) {
        _config = *config;
        const char *kernel_names[] = {"Floyd-Steinberg", "Jarvis", "Stucki", "Sierra-2-4A"};
        ESP_LOGI(TAG, "Dither config: kernel=%s, serpentine=%s",
                 kernel_names[_config.kernel], _config.serpentine ? "true" : "false");
        ESP_LOGI(TAG, "Palette: [%d,%d,%d] [%d,%d,%d] [%d,%d,%d] [%d,%d,%d] [%d,%d,%d] [%d,%d,%d]",
                 _config.palette[0][0], _config.palette[0][1], _config.palette[0][2],
                 _config.palette[1][0], _config.palette[1][1], _config.palette[1][2],
                 _config.palette[2][0], _config.palette[2][1], _config.palette[2][2],
                 _config.palette[3][0], _config.palette[3][1], _config.palette[3][2],
                 _config.palette[4][0], _config.palette[4][1], _config.palette[4][2],
                 _config.palette[5][0], _config.palette[5][1], _config.palette[5][2]);
    }
}

// ============================================================================
// Nearest Color (Perceptual)
// ============================================================================

int dither_engine::nearest_color_perceptual(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0;
    int best_dist = INT_MAX;

    for (int i = 0; i < 6; i++) {
        int dist = perceptual_distance(r, g, b,
                                        _config.palette[i][0],
                                        _config.palette[i][1],
                                        _config.palette[i][2]);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

// ============================================================================
// JPEG Decode
// ============================================================================

uint8_t dither_engine::Jpeg_decode(uint8_t *inbuffer, int inlen, uint8_t **outbuffer, int *outlen, int *width, int *height) {
    if (inbuffer == NULL) {
        ESP_LOGE(TAG, "jpeg_decode: inbuffer is NULL");
        return 0;
    }
    if (esp_jpeg_decode_one_picture(inbuffer, inlen, outbuffer, outlen, width, height) == JPEG_ERR_OK) {
        return 1;
    }
    return 0;
}

void dither_engine::Jpeg_dec_buffer_free(uint8_t *outbuffer) {
    if (outbuffer != NULL) {
        jpeg_free_align(outbuffer);
    }
}

// ============================================================================
// Main Dithering Function
// ============================================================================

void dither_engine::dither_rgb888(uint8_t *in_img, uint8_t *out_img, int w, int h) {
    // Allocate work buffer in PSRAM for large images
    uint8_t *work = (uint8_t *)heap_caps_malloc(w * h * 3, MALLOC_CAP_SPIRAM);
    if (!work) {
        // Fallback to internal RAM
        work = (uint8_t *)malloc(w * h * 3);
    }
    if (!work) {
        ESP_LOGE(TAG, "Failed to allocate work buffer for dithering");
        return;
    }

    // Copy input to work buffer
    memcpy(work, in_img, w * h * 3);

    // Select kernel based on config
    const int *weights;
    const int *offsets_x;
    const int *offsets_y;
    int divisor;
    int count;

    switch (_config.kernel) {
        case DITHER_JARVIS:
            weights = JARVIS_WEIGHTS;
            offsets_x = JARVIS_OFFSETS_X;
            offsets_y = JARVIS_OFFSETS_Y;
            divisor = JARVIS_DIVISOR;
            count = JARVIS_COUNT;
            break;
        case DITHER_STUCKI:
            weights = STUCKI_WEIGHTS;
            offsets_x = JARVIS_OFFSETS_X;  // Same offsets as Jarvis
            offsets_y = JARVIS_OFFSETS_Y;
            divisor = STUCKI_DIVISOR;
            count = STUCKI_COUNT;
            break;
        case DITHER_SIERRA_2_4A:
            weights = SIERRA_WEIGHTS;
            offsets_x = SIERRA_OFFSETS_X;
            offsets_y = SIERRA_OFFSETS_Y;
            divisor = SIERRA_DIVISOR;
            count = SIERRA_COUNT;
            break;
        default:  // DITHER_FLOYD_STEINBERG
            weights = FS_WEIGHTS;
            offsets_x = FS_OFFSETS_X;
            offsets_y = FS_OFFSETS_Y;
            divisor = FS_DIVISOR;
            count = FS_COUNT;
            break;
    }

    // Process each row
    for (int y = 0; y < h; y++) {
        // Serpentine scanning: alternate direction each row to reduce artifacts
        bool reverse = _config.serpentine && (y % 2 == 1);
        int x_start = reverse ? (w - 1) : 0;
        int x_end = reverse ? -1 : w;
        int x_step = reverse ? -1 : 1;

        for (int x = x_start; x != x_end; x += x_step) {
            int idx = (y * w + x) * 3;
            uint8_t r = work[idx];
            uint8_t g = work[idx + 1];
            uint8_t b = work[idx + 2];

            // Find nearest color using perceptual distance (uses calibrated palette)
            int ci = nearest_color_perceptual(r, g, b);

            // Output standard RGB values (for display compatibility)
            out_img[idx] = DEFAULT_PALETTE[ci][0];
            out_img[idx + 1] = DEFAULT_PALETTE[ci][1];
            out_img[idx + 2] = DEFAULT_PALETTE[ci][2];

            // Calculate quantization error using calibrated palette (for better dithering)
            int err_r = (int)r - _config.palette[ci][0];
            int err_g = (int)g - _config.palette[ci][1];
            int err_b = (int)b - _config.palette[ci][2];

            // Diffuse error to neighboring pixels
            // For serpentine scanning, flip the x offsets when going right-to-left
            for (int k = 0; k < count; k++) {
                int nx = x + (reverse ? -offsets_x[k] : offsets_x[k]);
                int ny = y + offsets_y[k];

                // Bounds check
                if (nx >= 0 && nx < w && ny < h) {
                    int n = (ny * w + nx) * 3;
                    work[n]     = CLAMP(work[n]     + err_r * weights[k] / divisor, 0, 255);
                    work[n + 1] = CLAMP(work[n + 1] + err_g * weights[k] / divisor, 0, 255);
                    work[n + 2] = CLAMP(work[n + 2] + err_b * weights[k] / divisor, 0, 255);
                }
            }
        }
    }

    // Free work buffer
    heap_caps_free(work);
}

// ============================================================================
// BMP File Save
// ============================================================================

int dither_engine::rgb888_to_sdcard_bmp(const char *filename, const uint8_t *rgb888, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("fopen");
        return -1;
    }

    // Each row must be aligned at 4-byte intervals (as required by BMP)
    int row_stride = (width * 3 + 3) & ~3;
    int img_size = row_stride * height;

    // Construct the file header
    BITMAPFILEHEADER file_header;
    file_header.bfType = 0x4D42;  // 'BM'
    file_header.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img_size;
    file_header.bfReserved1 = 0;
    file_header.bfReserved2 = 0;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    // Construction of information header
    BITMAPINFOHEADER info_header;
    memset(&info_header, 0, sizeof(info_header));
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = width;
    info_header.biHeight = height;  // Positive = Stored in reverse order (from bottom to top)
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = 0;  // BI_RGB
    info_header.biSizeImage = img_size;

    // Write the file header and information header
    fwrite(&file_header, sizeof(file_header), 1, f);
    fwrite(&info_header, sizeof(info_header), 1, f);

    // Write pixel data (BMP requires BGR order, each row is aligned at 4 bytes, and written in reverse order)
    uint8_t *row_buf = (uint8_t *)malloc(row_stride);
    if (!row_buf) {
        fclose(f);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        int src_row = height - 1 - y;  // Reverse order
        const uint8_t *src = rgb888 + src_row * width * 3;

        // Convert RGB888 -> BGR888
        for (int x = 0; x < width; x++) {
            row_buf[x * 3 + 0] = src[x * 3 + 2];  // B
            row_buf[x * 3 + 1] = src[x * 3 + 1];  // G
            row_buf[x * 3 + 2] = src[x * 3 + 0];  // R
        }
        // Fill aligned bytes
        for (int p = width * 3; p < row_stride; p++) {
            row_buf[p] = 0;
        }
        fwrite(row_buf, 1, row_stride, f);
    }

    free(row_buf);
    fclose(f);
    return 0;
}
