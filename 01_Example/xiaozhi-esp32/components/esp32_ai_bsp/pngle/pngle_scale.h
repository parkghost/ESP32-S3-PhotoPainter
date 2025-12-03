/**
 * @file pngle_scale.h
 * @brief PNG decoder with built-in downscaling support using pngle
 *
 * This wrapper provides PNG decoding with automatic downscaling to target
 * dimensions, using minimal memory by processing pixels as they are decoded.
 */

#ifndef PNGLE_SCALE_H
#define PNGLE_SCALE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scale mode for image fitting
 */
typedef enum {
    PNGLE_SCALE_FIT,    // Scale to fit within target, maintaining aspect ratio (letterbox)
    PNGLE_SCALE_FILL,   // Scale to fill target, maintaining aspect ratio (crop excess)
    PNGLE_SCALE_STRETCH // Stretch to exactly fit target dimensions
} pngle_scale_mode_t;

/**
 * @brief Result structure for PNG decoding
 */
typedef struct {
    uint8_t *rgb_buffer;    // Output RGB888 buffer (caller must free with free())
    int width;              // Actual output width
    int height;             // Actual output height
    int original_width;     // Original PNG width
    int original_height;    // Original PNG height
    size_t buffer_size;     // Size of rgb_buffer in bytes
} pngle_scale_result_t;

/**
 * @brief Decode PNG with automatic downscaling
 *
 * Decodes a PNG image and scales it to fit within the target dimensions.
 * The output buffer is allocated from SPIRAM for large images.
 *
 * @param png_data Pointer to PNG data
 * @param png_len Length of PNG data in bytes
 * @param target_width Target width (0 = use original width)
 * @param target_height Target height (0 = use original height)
 * @param scale_mode How to handle aspect ratio differences
 * @param result Output structure (caller must free result->rgb_buffer)
 * @return 0 on success, negative error code on failure
 */
int pngle_scale_decode(const uint8_t *png_data, size_t png_len,
                       int target_width, int target_height,
                       pngle_scale_mode_t scale_mode,
                       pngle_scale_result_t *result);

/**
 * @brief Get error message for error code
 *
 * @param error_code Error code from pngle_scale_decode
 * @return Human-readable error string
 */
const char *pngle_scale_error_text(int error_code);

// Error codes
#define PNGLE_SCALE_OK              0
#define PNGLE_SCALE_ERR_PARAM      -1
#define PNGLE_SCALE_ERR_MEMORY     -2
#define PNGLE_SCALE_ERR_PNG_INIT   -3
#define PNGLE_SCALE_ERR_PNG_DECODE -4

#ifdef __cplusplus
}
#endif

#endif // PNGLE_SCALE_H
