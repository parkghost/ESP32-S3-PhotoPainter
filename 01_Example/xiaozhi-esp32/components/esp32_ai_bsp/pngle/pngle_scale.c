/**
 * @file pngle_scale.c
 * @brief PNG decoder with built-in downscaling support
 */

#include "pngle_scale.h"
#include "pngle.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "PNGLE_SCALE";

// Context for decoding with scaling
typedef struct {
    uint8_t *rgb_buffer;        // Output buffer
    int target_width;           // Target output width
    int target_height;          // Target output height
    int original_width;         // Original PNG width
    int original_height;        // Original PNG height
    int output_width;           // Actual output width after scaling
    int output_height;          // Actual output height after scaling
    int offset_x;               // X offset for centering (FIT mode)
    int offset_y;               // Y offset for centering (FIT mode)
    float scale_x;              // Horizontal scale factor (output/input)
    float scale_y;              // Vertical scale factor (output/input)
    pngle_scale_mode_t scale_mode;
    int error_code;
    const char *error_msg;
} pngle_scale_ctx_t;

/**
 * @brief Init callback - called when PNG dimensions are known
 */
static void scale_init_callback(pngle_t *pngle, uint32_t w, uint32_t h) {
    pngle_scale_ctx_t *ctx = (pngle_scale_ctx_t *)pngle_get_user_data(pngle);

    ctx->original_width = w;
    ctx->original_height = h;

    ESP_LOGI(TAG, "PNG dimensions: %ux%u, target: %dx%d", w, h,
             ctx->target_width, ctx->target_height);

    // Calculate output dimensions based on scale mode
    int target_w = ctx->target_width > 0 ? ctx->target_width : w;
    int target_h = ctx->target_height > 0 ? ctx->target_height : h;

    float scale_w = (float)target_w / (float)w;
    float scale_h = (float)target_h / (float)h;

    switch (ctx->scale_mode) {
        case PNGLE_SCALE_FIT:
            // Use smaller scale to fit entirely within target
            if (scale_w < scale_h) {
                ctx->scale_x = ctx->scale_y = scale_w;
                ctx->output_width = target_w;
                ctx->output_height = (int)(h * scale_w);
            } else {
                ctx->scale_x = ctx->scale_y = scale_h;
                ctx->output_width = (int)(w * scale_h);
                ctx->output_height = target_h;
            }
            // Center the image
            ctx->offset_x = (target_w - ctx->output_width) / 2;
            ctx->offset_y = (target_h - ctx->output_height) / 2;
            // Allocate for full target size (with letterboxing)
            ctx->output_width = target_w;
            ctx->output_height = target_h;
            break;

        case PNGLE_SCALE_FILL:
            // Use larger scale to fill entire target (crop excess)
            if (scale_w > scale_h) {
                ctx->scale_x = ctx->scale_y = scale_w;
            } else {
                ctx->scale_x = ctx->scale_y = scale_h;
            }
            ctx->output_width = target_w;
            ctx->output_height = target_h;
            ctx->offset_x = 0;
            ctx->offset_y = 0;
            break;

        case PNGLE_SCALE_STRETCH:
            // Different scales for each axis
            ctx->scale_x = scale_w;
            ctx->scale_y = scale_h;
            ctx->output_width = target_w;
            ctx->output_height = target_h;
            ctx->offset_x = 0;
            ctx->offset_y = 0;
            break;
    }

    ESP_LOGI(TAG, "Output size: %dx%d, scale: %.3f x %.3f, offset: %d,%d",
             ctx->output_width, ctx->output_height,
             ctx->scale_x, ctx->scale_y,
             ctx->offset_x, ctx->offset_y);

    // Allocate RGB buffer from SPIRAM
    size_t buffer_size = ctx->output_width * ctx->output_height * 3;

    ESP_LOGI(TAG, "Allocating output buffer: %zu bytes, free SPIRAM: %d (largest: %d)",
             buffer_size,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ctx->rgb_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer!");
        ctx->error_code = PNGLE_SCALE_ERR_MEMORY;
        ctx->error_msg = "Failed to allocate output buffer";
        return;
    }

    // Initialize to black (for letterboxing in FIT mode)
    memset(ctx->rgb_buffer, 0, buffer_size);

    ESP_LOGI(TAG, "Output buffer allocated at %p", ctx->rgb_buffer);
}

/**
 * @brief Draw callback - called for each pixel
 *
 * For downscaling, we use area sampling by accumulating pixel values.
 * For simplicity, this implementation uses nearest-neighbor sampling
 * which is fast and works well for significant downscaling.
 */
static void scale_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    pngle_scale_ctx_t *ctx = (pngle_scale_ctx_t *)pngle_get_user_data(pngle);

    if (!ctx->rgb_buffer || ctx->error_code != 0) {
        return;
    }

    // Calculate output position
    int out_x, out_y;

    if (ctx->scale_mode == PNGLE_SCALE_FILL) {
        // For FILL mode, calculate position considering crop
        float scaled_w = ctx->original_width * ctx->scale_x;
        float scaled_h = ctx->original_height * ctx->scale_y;
        float crop_x = (scaled_w - ctx->output_width) / 2.0f;
        float crop_y = (scaled_h - ctx->output_height) / 2.0f;

        out_x = (int)(x * ctx->scale_x - crop_x);
        out_y = (int)(y * ctx->scale_y - crop_y);
    } else if (ctx->scale_mode == PNGLE_SCALE_FIT) {
        // For FIT mode, calculate position with offset
        float scale = ctx->scale_x;  // scale_x == scale_y in FIT mode
        int scaled_w = (int)(ctx->original_width * scale);
        int scaled_h = (int)(ctx->original_height * scale);
        int off_x = (ctx->output_width - scaled_w) / 2;
        int off_y = (ctx->output_height - scaled_h) / 2;

        out_x = (int)(x * scale) + off_x;
        out_y = (int)(y * scale) + off_y;
    } else {
        // STRETCH mode
        out_x = (int)(x * ctx->scale_x);
        out_y = (int)(y * ctx->scale_y);
    }

    // Bounds check
    if (out_x < 0 || out_x >= ctx->output_width ||
        out_y < 0 || out_y >= ctx->output_height) {
        return;
    }

    // Write RGB (ignore alpha for now)
    size_t offset = (out_y * ctx->output_width + out_x) * 3;
    ctx->rgb_buffer[offset + 0] = rgba[0];  // R
    ctx->rgb_buffer[offset + 1] = rgba[1];  // G
    ctx->rgb_buffer[offset + 2] = rgba[2];  // B
}

/**
 * @brief Done callback - called when decoding is complete
 */
static void scale_done_callback(pngle_t *pngle) {
    pngle_scale_ctx_t *ctx = (pngle_scale_ctx_t *)pngle_get_user_data(pngle);
    ESP_LOGI(TAG, "PNG decoding complete");
    (void)ctx;
}

int pngle_scale_decode(const uint8_t *png_data, size_t png_len,
                       int target_width, int target_height,
                       pngle_scale_mode_t scale_mode,
                       pngle_scale_result_t *result) {
    if (!png_data || png_len == 0 || !result) {
        return PNGLE_SCALE_ERR_PARAM;
    }

    // Initialize result
    memset(result, 0, sizeof(pngle_scale_result_t));

    // Create pngle instance
    pngle_t *pngle = pngle_new();
    if (!pngle) {
        ESP_LOGE(TAG, "Failed to create pngle instance");
        return PNGLE_SCALE_ERR_PNG_INIT;
    }

    // Setup context
    pngle_scale_ctx_t ctx = {
        .rgb_buffer = NULL,
        .target_width = target_width,
        .target_height = target_height,
        .original_width = 0,
        .original_height = 0,
        .output_width = 0,
        .output_height = 0,
        .offset_x = 0,
        .offset_y = 0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .scale_mode = scale_mode,
        .error_code = PNGLE_SCALE_OK,
        .error_msg = NULL
    };

    pngle_set_user_data(pngle, &ctx);
    pngle_set_init_callback(pngle, scale_init_callback);
    pngle_set_draw_callback(pngle, scale_draw_callback);
    pngle_set_done_callback(pngle, scale_done_callback);

    ESP_LOGI(TAG, "Starting PNG decode: %zu bytes input", png_len);

    // Feed all PNG data
    int fed = pngle_feed(pngle, png_data, png_len);

    if (fed < 0) {
        ESP_LOGE(TAG, "PNG decode error: %s", pngle_error(pngle));
        if (ctx.rgb_buffer) {
            heap_caps_free(ctx.rgb_buffer);
        }
        pngle_destroy(pngle);
        return PNGLE_SCALE_ERR_PNG_DECODE;
    }

    // Check for errors during decoding
    if (ctx.error_code != PNGLE_SCALE_OK) {
        ESP_LOGE(TAG, "Decode error: %s", ctx.error_msg ? ctx.error_msg : "unknown");
        if (ctx.rgb_buffer) {
            heap_caps_free(ctx.rgb_buffer);
        }
        pngle_destroy(pngle);
        return ctx.error_code;
    }

    // Verify we got a buffer
    if (!ctx.rgb_buffer) {
        ESP_LOGE(TAG, "No output buffer - decode may have failed");
        pngle_destroy(pngle);
        return PNGLE_SCALE_ERR_PNG_DECODE;
    }

    // Fill result
    result->rgb_buffer = ctx.rgb_buffer;
    result->width = ctx.output_width;
    result->height = ctx.output_height;
    result->original_width = ctx.original_width;
    result->original_height = ctx.original_height;
    result->buffer_size = ctx.output_width * ctx.output_height * 3;

    ESP_LOGI(TAG, "PNG decode complete: %dx%d -> %dx%d",
             result->original_width, result->original_height,
             result->width, result->height);

    pngle_destroy(pngle);
    return PNGLE_SCALE_OK;
}

const char *pngle_scale_error_text(int error_code) {
    switch (error_code) {
        case PNGLE_SCALE_OK:
            return "Success";
        case PNGLE_SCALE_ERR_PARAM:
            return "Invalid parameters";
        case PNGLE_SCALE_ERR_MEMORY:
            return "Memory allocation failed";
        case PNGLE_SCALE_ERR_PNG_INIT:
            return "Failed to initialize PNG decoder";
        case PNGLE_SCALE_ERR_PNG_DECODE:
            return "PNG decode error";
        default:
            return "Unknown error";
    }
}
