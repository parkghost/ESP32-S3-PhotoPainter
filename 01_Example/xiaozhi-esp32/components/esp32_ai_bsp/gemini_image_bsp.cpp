#include "gemini_image_bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "sdcard_bsp.h"
#include "pngle_scale.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Timing statistics structure
typedef struct {
    int64_t api_request_us;
    int64_t base64_decode_us;
    int64_t buffer_shrink_us;
    int64_t image_decode_us;
    int64_t resize_us;
    int64_t dither_us;
    int64_t save_bmp_us;
    int64_t total_us;
    // File info
    size_t base64_len;
    size_t decoded_file_size;
    size_t rgb_buffer_size;
    int original_width;
    int original_height;
    int target_width;
    int target_height;
    const char *image_format;
} image_gen_stats_t;



#define TAG "GEMINI_IMG"

// Image models that support tools (e.g., google_search)
static const char* MODELS_WITH_TOOLS[] = {
    "gemini-3-pro-image-preview",
    NULL  // Sentinel
};

// Image models that support imageSize parameter
static const char* MODELS_WITH_IMAGE_SIZE[] = {
    "gemini-3-pro-image-preview",
    NULL  // Sentinel
};

// Check if a model supports tools
static bool model_supports_tools(const char* model) {
    for (int i = 0; MODELS_WITH_TOOLS[i] != NULL; i++) {
        if (strstr(model, MODELS_WITH_TOOLS[i]) != NULL) {
            return true;
        }
    }
    return false;
}

// Check if a model supports imageSize parameter
static bool model_supports_image_size(const char* model) {
    for (int i = 0; MODELS_WITH_IMAGE_SIZE[i] != NULL; i++) {
        if (strstr(model, MODELS_WITH_IMAGE_SIZE[i]) != NULL) {
            return true;
        }
    }
    return false;
}

// Gemini API endpoint
#define GEMINI_API_URL "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s"

// Base64 decode table
const int8_t gemini_image_bsp::base64_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

gemini_image_bsp::gemini_image_bsp(const char *ai_model, const char *gemini_api_key, const int width, const int height) {
    _width        = width;
    _height       = height;
    _aspect_ratio = ASPECT_RATIO_16_9;  // Default to landscape
    _scale_mode   = SCALE_MODE_FILL;    // Default to fill (crop excess)
    model         = ai_model;
    api_key       = gemini_api_key;
    // Allocate buffers in SPIRAM
    request_body  = (char *) heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM);
    png_buffer    = (uint8_t *) heap_caps_malloc(width * height * 4, MALLOC_CAP_SPIRAM);  // RGBA
    floyd_buffer  = (uint8_t *) heap_caps_malloc(width * height * 3, MALLOC_CAP_SPIRAM);  // RGB888
    assert(request_body);
    assert(png_buffer);
    assert(floyd_buffer);
}

gemini_image_bsp::~gemini_image_bsp() {
    if (request_body) heap_caps_free(request_body);
    if (png_buffer) heap_caps_free(png_buffer);
    if (floyd_buffer) heap_caps_free(floyd_buffer);
}

void gemini_image_bsp::set_AspectRatio(gemini_aspect_ratio_t ratio) {
    _aspect_ratio = ratio;
    ESP_LOGI(TAG, "Aspect ratio set to: %s", ratio == ASPECT_RATIO_16_9 ? "16:9 (Landscape)" : "9:16 (Portrait)");
}

gemini_aspect_ratio_t gemini_image_bsp::get_AspectRatio() const {
    return _aspect_ratio;
}

void gemini_image_bsp::set_ScaleMode(scale_mode_t mode) {
    _scale_mode = mode;
    ESP_LOGI(TAG, "Scale mode set to: %s", mode == SCALE_MODE_FILL ? "fill (crop excess)" : "fit (pad with white)");
}

scale_mode_t gemini_image_bsp::get_ScaleMode() const {
    return _scale_mode;
}

// Dynamic response buffer allocation
// Will allocate based on available SPIRAM, leaving 512KB for other operations
#define MIN_RESPONSE_BUFFER_SIZE (2 * 1024 * 1024)  // 2MB minimum
#define SPIRAM_RESERVE_SIZE (512 * 1024)  // Keep 512KB free for other ops

// Store actual allocated buffer size for overflow checking
static size_t g_response_buffer_size = 0;

int gemini_image_bsp::_http_event_handler(esp_http_client_event_t *evt) {
    gemini_http_response_t *resp = (gemini_http_response_t *) evt->user_data;

    // Debug: Log all HTTP events
    static int chunk_count = 0;
    static size_t total_received = 0;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "[HTTP] ERROR event");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "[HTTP] Connected to server");
        chunk_count = 0;
        total_received = 0;
        break;
    case HTTP_EVENT_HEADERS_SENT:
        ESP_LOGI(TAG, "[HTTP] Headers sent");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "[HTTP] Header: %s = %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        // Handle both chunked and non-chunked responses
        {
            chunk_count++;
            // Log progress every 50 chunks or first 5 chunks
            if (chunk_count <= 5 || chunk_count % 50 == 0) {
                ESP_LOGD(TAG, "[HTTP] Data chunk #%d, size: %d bytes, total so far: %zu",
                         chunk_count, evt->data_len, total_received);
            }

            // Pre-allocate buffer on first data chunk based on available memory
            if (resp->buffer == NULL) {
                ESP_LOGD(TAG, "[HTTP] First chunk - allocating buffer...");
                size_t free_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                size_t alloc_size = free_spiram > SPIRAM_RESERVE_SIZE ?
                                    free_spiram - SPIRAM_RESERVE_SIZE : 0;

                ESP_LOGD(TAG, "[HTTP] Free SPIRAM block: %zu, alloc_size: %zu", free_spiram, alloc_size);

                if (alloc_size < MIN_RESPONSE_BUFFER_SIZE) {
                    ESP_LOGE(TAG, "Not enough SPIRAM: %zu available, need %d",
                             free_spiram, MIN_RESPONSE_BUFFER_SIZE);
                    return ESP_FAIL;
                }

                resp->buffer = (char *) heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
                if (resp->buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer (%zu bytes)", alloc_size);
                    return ESP_FAIL;
                }
                resp->buffer_len = 0;
                g_response_buffer_size = alloc_size;
                ESP_LOGD(TAG, "Allocated %zu bytes for response buffer (free: %zu)",
                         alloc_size, free_spiram);
            }

            int copy_len = evt->data_len;
            // Check if we have enough space
            if (resp->buffer_len + copy_len >= g_response_buffer_size) {
                ESP_LOGE(TAG, "Response buffer overflow (%d + %d >= %zu), truncating",
                         resp->buffer_len, copy_len, g_response_buffer_size);
                copy_len = g_response_buffer_size - resp->buffer_len - 1;
                if (copy_len <= 0) return ESP_OK;
            }

            memcpy(resp->buffer + resp->buffer_len, evt->data, copy_len);
            resp->buffer_len += copy_len;
            resp->buffer[resp->buffer_len] = '\0';
            total_received = resp->buffer_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "[HTTP] Finished - total chunks: %d, total bytes: %zu", chunk_count, total_received);
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "[HTTP] Disconnected");
        break;
    default:
        ESP_LOGD(TAG, "[HTTP] Unknown event: %d", evt->event_id);
        break;
    }
    return ESP_OK;
}

int gemini_image_bsp::base64_decode(const char *input, size_t input_len, uint8_t *output, size_t *output_len) {
    size_t out_idx = 0;
    uint32_t accum = 0;
    int bits = 0;

    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;

        int8_t val = base64_decode_table[(uint8_t)c];
        if (val < 0) {
            ESP_LOGE(TAG, "Invalid base64 character: %c", c);
            return -1;
        }

        accum = (accum << 6) | val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            output[out_idx++] = (accum >> bits) & 0xFF;
        }
    }

    *output_len = out_idx;
    return 0;
}

// Helper for resizing image with aspect ratio preservation
// Fill mode: scale to fill target, crop excess (no distortion, may lose edges)
// Fit mode: scale to fit within target, pad with white (no distortion, shows all content)
static void resize_nearest_rgb888(const uint8_t* src, int src_w, int src_h,
                                   uint8_t* dst, int dst_w, int dst_h,
                                   scale_mode_t mode = SCALE_MODE_FILL) {
    // Calculate aspect ratios
    float src_aspect = (float)src_w / src_h;
    float dst_aspect = (float)dst_w / dst_h;

    int scaled_w, scaled_h;
    int offset_x = 0, offset_y = 0;
    int crop_x = 0, crop_y = 0;

    if (mode == SCALE_MODE_FILL) {
        // Fill mode: scale to fill entire target, crop excess
        if (src_aspect > dst_aspect) {
            // Source is wider - scale by height, crop width
            scaled_h = dst_h;
            scaled_w = (int)(src_w * ((float)dst_h / src_h));
            crop_x = (scaled_w - dst_w) / 2;  // Crop from center
        } else {
            // Source is taller - scale by width, crop height
            scaled_w = dst_w;
            scaled_h = (int)(src_h * ((float)dst_w / src_w));
            crop_y = (scaled_h - dst_h) / 2;  // Crop from center
        }

        ESP_LOGI("resize", "Fill mode: %dx%d -> scale to %dx%d, crop (%d,%d), output %dx%d",
                 src_w, src_h, scaled_w, scaled_h, crop_x, crop_y, dst_w, dst_h);

        // Sample from source with cropping
        for (int y = 0; y < dst_h; y++) {
            int scaled_y = y + crop_y;
            int src_y = scaled_y * src_h / scaled_h;
            if (src_y >= src_h) src_y = src_h - 1;

            for (int x = 0; x < dst_w; x++) {
                int scaled_x = x + crop_x;
                int src_x = scaled_x * src_w / scaled_w;
                if (src_x >= src_w) src_x = src_w - 1;

                int src_idx = (src_y * src_w + src_x) * 3;
                int dst_idx = (y * dst_w + x) * 3;
                dst[dst_idx + 0] = src[src_idx + 0];
                dst[dst_idx + 1] = src[src_idx + 1];
                dst[dst_idx + 2] = src[src_idx + 2];
            }
        }
    } else {
        // Fit mode: scale to fit within target, pad with white
        if (src_aspect > dst_aspect) {
            // Source is wider - scale by width, pad height
            scaled_w = dst_w;
            scaled_h = (int)(src_h * ((float)dst_w / src_w));
            offset_y = (dst_h - scaled_h) / 2;  // Center vertically
        } else {
            // Source is taller - scale by height, pad width
            scaled_h = dst_h;
            scaled_w = (int)(src_w * ((float)dst_h / src_h));
            offset_x = (dst_w - scaled_w) / 2;  // Center horizontally
        }

        ESP_LOGI("resize", "Fit mode: %dx%d -> scale to %dx%d, offset (%d,%d), output %dx%d",
                 src_w, src_h, scaled_w, scaled_h, offset_x, offset_y, dst_w, dst_h);

        // Fill entire destination with white first
        memset(dst, 255, dst_w * dst_h * 3);

        // Sample from source and place at offset
        for (int y = 0; y < scaled_h; y++) {
            int src_y = y * src_h / scaled_h;
            if (src_y >= src_h) src_y = src_h - 1;
            int dst_y = y + offset_y;

            for (int x = 0; x < scaled_w; x++) {
                int src_x = x * src_w / scaled_w;
                if (src_x >= src_w) src_x = src_w - 1;
                int dst_x = x + offset_x;

                int src_idx = (src_y * src_w + src_x) * 3;
                int dst_idx = (dst_y * dst_w + dst_x) * 3;
                dst[dst_idx + 0] = src[src_idx + 0];
                dst[dst_idx + 1] = src[src_idx + 1];
                dst[dst_idx + 2] = src[src_idx + 2];
            }
        }
    }
}

const char* gemini_image_bsp::gemini_generate_image(bool skip_sd_save) {
    gemini_http_response_t response = {0};
    image_gen_stats_t stats = {0};
    int64_t start_time, end_time, total_start_time;

    total_start_time = esp_timer_get_time();

    // Debug: Show available memory before starting
    ESP_LOGI(TAG, "=== Starting Gemini Image Generation ===");
    ESP_LOGI(TAG, "Free SPIRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free internal: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Largest SPIRAM block: %d bytes", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Temporarily free pre-allocated buffers to maximize memory for API response
    // This gives us an extra ~2.6MB of SPIRAM for the response buffer
    if (png_buffer) {
        heap_caps_free(png_buffer);
        png_buffer = NULL;
        ESP_LOGI(TAG, "Freed png_buffer to increase response buffer capacity");
    }
    if (floyd_buffer) {
        heap_caps_free(floyd_buffer);
        floyd_buffer = NULL;
        ESP_LOGI(TAG, "Freed floyd_buffer to increase response buffer capacity");
    }
    ESP_LOGI(TAG, "After freeing buffers - Largest SPIRAM block: %d bytes",
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Build full URL with model and API key
    char url[512];
    snprintf(url, sizeof(url), GEMINI_API_URL, model, api_key);

    esp_http_client_config_t config = {};
    config.url              = url;
    config.event_handler    = _http_event_handler;
    config.user_data        = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // Use ESP-IDF certificate bundle
    config.timeout_ms       = 60000;  // 60 second timeout for image generation
    config.buffer_size      = 8192;
    config.buffer_size_tx   = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    ESP_LOGI(TAG, "Calling Gemini API: %s", model);
    ESP_LOGI(TAG, "Request body (%d bytes):", strlen(request_body));
    printf("%s\n", request_body);

    // === TIMING: API Request ===
    ESP_LOGI(TAG, "[DEBUG] >>> Starting HTTP perform...");
    ESP_LOGI(TAG, "[DEBUG] Free heap: internal=%d, SPIRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    start_time = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    end_time = esp_timer_get_time();

    ESP_LOGI(TAG, "[DEBUG] <<< HTTP perform returned: %s (0x%x)", esp_err_to_name(err), err);

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[DEBUG] HTTP status code: %d", status_code);

    stats.api_request_us = end_time - start_time;
    ESP_LOGI(TAG, "[TIMING] API request: %lld ms", stats.api_request_us / 1000);

    ESP_LOGI(TAG, "[DEBUG] Response buffer: %p, len: %d",
             response.buffer, response.buffer_len);

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "[DEBUG] HTTP client cleaned up");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gemini request failed: %s", esp_err_to_name(err));
        heap_caps_free(response.buffer);
        return NULL;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "Gemini API error, status: %d", status_code);
        ESP_LOGE(TAG, "Response: %s", response.buffer ? response.buffer : "NULL");
        heap_caps_free(response.buffer);
        return NULL;
    }

    ESP_LOGI(TAG, "Gemini response received, parsing manually (buffer size: %d)...", response.buffer_len);
    ESP_LOGI(TAG, "Free SPIRAM after response: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Debug: Print first 200 chars of response to see structure
    if (response.buffer_len > 0) {
        char debug_buf[201];
        size_t copy_len = response.buffer_len < 200 ? response.buffer_len : 200;
        memcpy(debug_buf, response.buffer, copy_len);
        debug_buf[copy_len] = '\0';
        ESP_LOGI(TAG, "Response preview: %s", debug_buf);
    }

    // Manual JSON parsing to avoid ArduinoJson memory issues
    // Response format: { "candidates": [{ "content": { "parts": [{ "inlineData": { "mimeType": "image/png", "data": "base64..." }}]}}]}

    // Find "inlineData" section
    const char *inline_data = strstr(response.buffer, "\"inlineData\"");
    if (inline_data == NULL) {
        ESP_LOGE(TAG, "No inlineData in response");
        heap_caps_free(response.buffer);
        return NULL;
    }

    // Find mimeType within inlineData
    const char *mime_start = strstr(inline_data, "\"mimeType\"");
    char mime_type_str[32] = "unknown";
    if (mime_start != NULL) {
        mime_start = strchr(mime_start + 10, '"');  // Skip to value
        if (mime_start != NULL) {
            mime_start++;  // Skip opening quote
            const char *mime_end = strchr(mime_start, '"');
            if (mime_end != NULL) {
                size_t mime_len = mime_end - mime_start;
                if (mime_len < sizeof(mime_type_str)) {
                    memcpy(mime_type_str, mime_start, mime_len);
                    mime_type_str[mime_len] = '\0';
                }
            }
        }
    }
    ESP_LOGI(TAG, "Image received, MIME type: %s", mime_type_str);

    // Find "data" field - search for "data":" pattern
    const char *data_key = strstr(inline_data, "\"data\"");
    if (data_key == NULL) {
        ESP_LOGE(TAG, "No data field in inlineData");
        heap_caps_free(response.buffer);
        return NULL;
    }

    // Find the colon and opening quote
    const char *data_start = strchr(data_key + 6, '"');
    if (data_start == NULL) {
        ESP_LOGE(TAG, "Malformed data field");
        heap_caps_free(response.buffer);
        return NULL;
    }
    data_start++;  // Skip opening quote

    // Find the closing quote for base64 data
    const char *data_end = strchr(data_start, '"');
    if (data_end == NULL) {
        ESP_LOGE(TAG, "No closing quote for data");
        heap_caps_free(response.buffer);
        return NULL;
    }

    size_t base64_len = data_end - data_start;
    stats.base64_len = base64_len;
    ESP_LOGI(TAG, "Found base64 data, length: %zu", base64_len);

    // === TIMING: Base64 Decode ===
    start_time = esp_timer_get_time();

    // Decode base64 in-place into the response buffer to save memory
    // This is safe because base64 decodes to 3/4 the size, so the write position
    // (starting at buffer[0]) will never catch up with the read position (data_start)
    size_t decoded_len = 0;
    uint8_t *decoded_buffer = (uint8_t *) response.buffer;  // Reuse response buffer

    if (base64_decode(data_start, base64_len, decoded_buffer, &decoded_len) != 0) {
        ESP_LOGE(TAG, "Base64 decode failed");
        heap_caps_free(response.buffer);
        return NULL;
    }

    end_time = esp_timer_get_time();
    stats.base64_decode_us = end_time - start_time;
    stats.decoded_file_size = decoded_len;
    ESP_LOGI(TAG, "[TIMING] Base64 decode: %lld ms", stats.base64_decode_us / 1000);
    ESP_LOGI(TAG, "Decoded image size: %zu bytes (in-place)", decoded_len);

    // Debug: Show first 16 bytes of decoded data (magic bytes)
    if (decoded_len >= 16) {
        ESP_LOGI(TAG, "First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 decoded_buffer[0], decoded_buffer[1], decoded_buffer[2], decoded_buffer[3],
                 decoded_buffer[4], decoded_buffer[5], decoded_buffer[6], decoded_buffer[7],
                 decoded_buffer[8], decoded_buffer[9], decoded_buffer[10], decoded_buffer[11],
                 decoded_buffer[12], decoded_buffer[13], decoded_buffer[14], decoded_buffer[15]);
    }

    // Check if it's PNG or JPEG based on magic bytes
    bool is_png = (decoded_len > 8 && decoded_buffer[0] == 0x89 && decoded_buffer[1] == 0x50);
    bool is_jpeg = (decoded_len > 2 && decoded_buffer[0] == 0xFF && decoded_buffer[1] == 0xD8);
    stats.image_format = is_jpeg ? "JPEG" : (is_png ? "PNG" : "Unknown");
    ESP_LOGI(TAG, "Image format detection: %s (is_png=%d, is_jpeg=%d)", stats.image_format, is_png, is_jpeg);

    // === TIMING: Buffer Relocation ===
    start_time = esp_timer_get_time();

    // IMPORTANT: Don't use realloc to shrink - it causes memory fragmentation!
    // Instead: allocate new smaller buffer, copy data, then free the large buffer.
    // This ensures the original 4MB block is freed as one contiguous piece.
    uint8_t *new_buffer = (uint8_t *) heap_caps_malloc(decoded_len, MALLOC_CAP_SPIRAM);
    if (new_buffer != NULL) {
        memcpy(new_buffer, decoded_buffer, decoded_len);
        heap_caps_free(response.buffer);  // Free the large 4MB buffer completely
        decoded_buffer = new_buffer;
        ESP_LOGI(TAG, "Relocated PNG data: freed %zu bytes, allocated %zu bytes (defragmented)",
                 g_response_buffer_size, decoded_len);
    } else {
        // Fallback: try realloc if new allocation fails
        ESP_LOGW(TAG, "New buffer allocation failed, trying realloc (may fragment)");
        uint8_t *shrunk_buffer = (uint8_t *) heap_caps_realloc(response.buffer, decoded_len, MALLOC_CAP_SPIRAM);
        if (shrunk_buffer != NULL) {
            decoded_buffer = shrunk_buffer;
        } else {
            decoded_buffer = (uint8_t *) response.buffer;
        }
    }
    response.buffer = NULL;  // Ownership transferred to decoded_buffer

    end_time = esp_timer_get_time();
    stats.buffer_shrink_us = end_time - start_time;
    ESP_LOGI(TAG, "[TIMING] Buffer relocation: %lld ms", stats.buffer_shrink_us / 1000);

    uint8_t *rgb_buffer = NULL;
    int rgb_len = 0;
    int img_w = 0;
    int img_h = 0;

    ESP_LOGI(TAG, "Free SPIRAM before image decode: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // === TIMING: Image Decode ===
    start_time = esp_timer_get_time();

    if (is_jpeg) {
        ESP_LOGI(TAG, "Decoding JPEG image (size: %zu bytes)...", decoded_len);
        // Jpeg_decode returns 1 on success, 0 on failure
        int jpeg_result = Jpeg_decode(decoded_buffer, decoded_len, &rgb_buffer, &rgb_len, &img_w, &img_h);
        ESP_LOGI(TAG, "JPEG decode result: %d (1=OK), rgb_len: %d, size: %dx%d", jpeg_result, rgb_len, img_w, img_h);
        if (jpeg_result == 0) {
            ESP_LOGE(TAG, "JPEG decode failed");
            heap_caps_free(decoded_buffer);
            return NULL;
        }
    } else if (is_png) {
        ESP_LOGI(TAG, "Decoding PNG image (size: %zu bytes)...", decoded_len);
        int png_result = png_to_rgb888(decoded_buffer, decoded_len, &rgb_buffer, &rgb_len, &img_w, &img_h);
        ESP_LOGI(TAG, "PNG decode result: %d, rgb_len: %d, size: %dx%d", png_result, rgb_len, img_w, img_h);
        if (png_result == 0) {
            ESP_LOGE(TAG, "PNG decode failed");
            heap_caps_free(decoded_buffer);
            return NULL;
        }
    } else {
        ESP_LOGE(TAG, "Unknown image format (not PNG 0x89 0x50 or JPEG 0xFF 0xD8)");
        heap_caps_free(decoded_buffer);
        return NULL;
    }

    end_time = esp_timer_get_time();
    stats.image_decode_us = end_time - start_time;
    stats.original_width = img_w;
    stats.original_height = img_h;
    stats.rgb_buffer_size = rgb_len;
    ESP_LOGI(TAG, "[TIMING] Image decode (%s): %lld ms", stats.image_format, stats.image_decode_us / 1000);

    ESP_LOGI(TAG, "Image decoded to RGB, size: %d bytes", rgb_len);
    ESP_LOGI(TAG, "Free SPIRAM after image decode: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    heap_caps_free(decoded_buffer);

    // Re-allocate floyd_buffer now that response buffer is freed
    // This buffer is needed for resize and dithering operations
    floyd_buffer = (uint8_t *) heap_caps_malloc(_width * _height * 3, MALLOC_CAP_SPIRAM);
    if (floyd_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to re-allocate floyd_buffer (%d bytes)", _width * _height * 3);
        if (is_jpeg) Jpeg_dec_buffer_free(rgb_buffer);
        else heap_caps_free(rgb_buffer);
        return NULL;
    }
    ESP_LOGI(TAG, "Re-allocated floyd_buffer: %d bytes", _width * _height * 3);

    // Resize logic - swap dimensions for portrait mode
    uint8_t *dither_input = rgb_buffer;
    bool used_internal_resize = false;
    int target_w, target_h;
    if (_aspect_ratio == ASPECT_RATIO_9_16) {
        // Portrait mode: swap width and height
        target_w = _height;  // 480
        target_h = _width;   // 800
    } else {
        // Landscape mode: use original dimensions
        target_w = _width;   // 800
        target_h = _height;  // 480
    }
    stats.target_width = target_w;
    stats.target_height = target_h;

    // === TIMING: Resize ===
    start_time = esp_timer_get_time();

    if (img_w != target_w || img_h != target_h) {
        ESP_LOGI(TAG, "Resizing image from %dx%d to %dx%d (scale_mode=%s)",
                 img_w, img_h, target_w, target_h,
                 _scale_mode == SCALE_MODE_FIT ? "fit" : "fill");
        resize_nearest_rgb888(rgb_buffer, img_w, img_h, floyd_buffer, target_w, target_h, _scale_mode);

        // Free original buffer
        if (is_jpeg) Jpeg_dec_buffer_free(rgb_buffer);
        else heap_caps_free(rgb_buffer);

        dither_input = floyd_buffer;
        used_internal_resize = true;

        end_time = esp_timer_get_time();
        stats.resize_us = end_time - start_time;
        ESP_LOGI(TAG, "[TIMING] Resize (%dx%d -> %dx%d): %lld ms", img_w, img_h, target_w, target_h, stats.resize_us / 1000);
    } else {
        stats.resize_us = 0;
        ESP_LOGI(TAG, "[TIMING] Resize: skipped (same size)");
    }

    // === TIMING: Floyd-Steinberg Dithering ===
    start_time = esp_timer_get_time();

    // Apply Floyd-Steinberg dithering
    ESP_LOGI(TAG, "Applying Floyd-Steinberg dithering (target: %dx%d)...", target_w, target_h);
    dither_fs_rgb888(dither_input, floyd_buffer, target_w, target_h);

    end_time = esp_timer_get_time();
    stats.dither_us = end_time - start_time;
    ESP_LOGI(TAG, "[TIMING] Floyd-Steinberg dithering: %lld ms", stats.dither_us / 1000);
    ESP_LOGI(TAG, "Dithering complete");

    // Store dimensions for direct display accessor
    _last_target_w = target_w;
    _last_target_h = target_h;

    // Free the RGB buffer if not reused
    if (!used_internal_resize) {
        if (is_jpeg) {
            Jpeg_dec_buffer_free(dither_input);
        } else {
            heap_caps_free(dither_input);
        }
    }
    ESP_LOGI(TAG, "Free SPIRAM after dithering: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // === TIMING: Save BMP (skip if direct display mode) ===
    if (!skip_sd_save) {
        start_time = esp_timer_get_time();

        // Save to SD card as BMP
        snprintf(sdcard_path, 98, "/sdcard/05_user_ai_img/ai_%d.bmp", path_value);
        ESP_LOGI(TAG, "Saving to: %s", sdcard_path);

        // Save BMP in original orientation (portrait or landscape)
        // The display code will detect and handle rotation
        int save_result = rgb888_to_sdcard_bmp(sdcard_path, floyd_buffer, target_w, target_h);
        if (save_result != 0) {
            ESP_LOGE(TAG, "Failed to save BMP to SD card, error: %d", save_result);
            return NULL;
        }

        end_time = esp_timer_get_time();
        stats.save_bmp_us = end_time - start_time;
        ESP_LOGI(TAG, "[TIMING] Save BMP to SD card: %lld ms", stats.save_bmp_us / 1000);
    } else {
        ESP_LOGI(TAG, "Direct display mode: skipping SD card save");
        stats.save_bmp_us = 0;
    }

    // Calculate total time
    stats.total_us = esp_timer_get_time() - total_start_time;

    // === TIMING SUMMARY ===
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║           IMAGE GENERATION TIMING SUMMARY                    ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Stage                    │ Time (ms)  │ Percentage           ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ API Request              │ %10lld │ %5.1f%%               ║",
             stats.api_request_us / 1000, (float)stats.api_request_us / stats.total_us * 100);
    ESP_LOGI(TAG, "║ Base64 Decode            │ %10lld │ %5.1f%%               ║",
             stats.base64_decode_us / 1000, (float)stats.base64_decode_us / stats.total_us * 100);
    ESP_LOGI(TAG, "║ Buffer Shrink            │ %10lld │ %5.1f%%               ║",
             stats.buffer_shrink_us / 1000, (float)stats.buffer_shrink_us / stats.total_us * 100);
    ESP_LOGI(TAG, "║ Image Decode (%s)      │ %10lld │ %5.1f%%               ║",
             stats.image_format, stats.image_decode_us / 1000, (float)stats.image_decode_us / stats.total_us * 100);
    ESP_LOGI(TAG, "║ Resize                   │ %10lld │ %5.1f%%               ║",
             stats.resize_us / 1000, (float)stats.resize_us / stats.total_us * 100);
    ESP_LOGI(TAG, "║ Floyd-Steinberg Dither   │ %10lld │ %5.1f%%               ║",
             stats.dither_us / 1000, (float)stats.dither_us / stats.total_us * 100);
    if (!skip_sd_save) {
        ESP_LOGI(TAG, "║ Save BMP to SD           │ %10lld │ %5.1f%%               ║",
                 stats.save_bmp_us / 1000, (float)stats.save_bmp_us / stats.total_us * 100);
    }
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ TOTAL                    │ %10lld │ 100.0%%               ║", stats.total_us / 1000);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║           FILE INFORMATION                                   ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Image Format:        %s                                    ║", stats.image_format);
    ESP_LOGI(TAG, "║ Base64 Length:       %zu bytes                              ║", stats.base64_len);
    ESP_LOGI(TAG, "║ Decoded File Size:   %zu bytes (%.2f KB)                    ║",
             stats.decoded_file_size, (float)stats.decoded_file_size / 1024);
    ESP_LOGI(TAG, "║ RGB Buffer Size:     %zu bytes (%.2f MB)                    ║",
             stats.rgb_buffer_size, (float)stats.rgb_buffer_size / (1024 * 1024));
    ESP_LOGI(TAG, "║ Original Resolution: %d x %d                                ║",
             stats.original_width, stats.original_height);
    ESP_LOGI(TAG, "║ Target Resolution:   %d x %d                                ║",
             stats.target_width, stats.target_height);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    if (skip_sd_save) {
        ESP_LOGI(TAG, "=== Image Generation Complete! (Direct display mode) ===");
    } else {
        ESP_LOGI(TAG, "=== Image Generation Complete! Saved to %s ===", sdcard_path);
        path_value++;
    }
    return sdcard_path;
}

uint8_t gemini_image_bsp::png_to_rgb888(uint8_t *png_data, int png_len, uint8_t **rgb_buffer, int *rgb_len, int *width, int *height) {
    // Calculate target dimensions based on aspect ratio
    int target_w, target_h;
    if (_aspect_ratio == ASPECT_RATIO_9_16) {
        target_w = _height;  // 480
        target_h = _width;   // 800
    } else {
        target_w = _width;   // 800
        target_h = _height;  // 480
    }

    // First, read PNG dimensions from IHDR chunk
    unsigned preview_w = 0, preview_h = 0;
    if (png_len >= 24 && png_data[0] == 0x89 && png_data[1] == 0x50 &&
        png_data[2] == 0x4E && png_data[3] == 0x47) {
        preview_w = (png_data[16] << 24) | (png_data[17] << 16) |
                    (png_data[18] << 8) | png_data[19];
        preview_h = (png_data[20] << 24) | (png_data[21] << 16) |
                    (png_data[22] << 8) | png_data[23];
        ESP_LOGI(TAG, "PNG dimensions: %ux%u, target: %dx%d", preview_w, preview_h, target_w, target_h);
    }

    // Check if PNG needs scaling (larger than target)
    bool needs_scaling = (preview_w > (unsigned)target_w || preview_h > (unsigned)target_h);

    // Map scale mode
    pngle_scale_mode_t pngle_mode = (_scale_mode == SCALE_MODE_FILL) ?
                                     PNGLE_SCALE_FILL : PNGLE_SCALE_FIT;

    pngle_scale_result_t result;
    int err;

    if (needs_scaling) {
        // Use pngle streaming decoder with built-in scaling
        // This avoids allocating a huge buffer for the full-size image
        ESP_LOGI(TAG, "Using pngle with scaling (%ux%u -> %dx%d)",
                 preview_w, preview_h, target_w, target_h);
        err = pngle_scale_decode(png_data, png_len, target_w, target_h, pngle_mode, &result);
    } else {
        // For images that don't need scaling, decode at original size
        ESP_LOGI(TAG, "Using pngle for same-size decode (%ux%u)", preview_w, preview_h);
        err = pngle_scale_decode(png_data, png_len, 0, 0, PNGLE_SCALE_STRETCH, &result);
    }

    if (err != PNGLE_SCALE_OK) {
        ESP_LOGE(TAG, "pngle decode failed: %s", pngle_scale_error_text(err));
        return 0;
    }

    *rgb_buffer = result.rgb_buffer;
    *rgb_len = result.buffer_size;
    if (width) *width = result.width;
    if (height) *height = result.height;

    ESP_LOGI(TAG, "PNG decoded: %dx%d (original: %dx%d)",
             result.width, result.height, result.original_width, result.original_height);
    return 1;
}

void gemini_image_bsp::set_Chat(const char *str) {
    // Build Gemini API request body
    // Request format for image generation with responseModalities
    doc.clear();

    // Determine aspect ratio string and resolution hint based on setting
    const char *aspect_ratio_str = (_aspect_ratio == ASPECT_RATIO_16_9) ? "16:9" : "9:16";

    // Create contents array with text prompt + size hint
    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject content = contents.add<JsonObject>();
    JsonArray parts = content["parts"].to<JsonArray>();
    JsonObject textPart = parts.add<JsonObject>();
    textPart["text"] = str;

    // Generation config for image output
    JsonObject genConfig = doc["generationConfig"].to<JsonObject>();
    JsonArray modalities = genConfig["responseModalities"].to<JsonArray>();
    modalities.add("TEXT");
    modalities.add("IMAGE");

    // Image config - request specific aspect ratio and size
    JsonObject imageConfig = genConfig["imageConfig"].to<JsonObject>();
    // Use configured aspect ratio
    imageConfig["aspectRatio"] = aspect_ratio_str;
    // Use 1K resolution to reduce memory usage and speed up transfer
    // Note: Only some models support imageSize (see MODELS_WITH_IMAGE_SIZE)
    if (model_supports_image_size(model)) {
        imageConfig["imageSize"] = "1K";
    }

    // Enable Google Search for grounded generation (up-to-date information)
    // Note: Some image models do not support tools (see MODELS_WITHOUT_TOOLS)
    if (model_supports_tools(model)) {
        JsonArray tools = doc["tools"].to<JsonArray>();
        JsonObject searchTool = tools.add<JsonObject>();
        searchTool["google_search"] = JsonObject();
    }

    if (serializeJson(doc, request_body, 4 * 1024) > 0) {
        is_success = true;
        ESP_LOGI(TAG, "=== Gemini API Request Parameters ===");
        ESP_LOGI(TAG, "Model: %s", model);
        ESP_LOGI(TAG, "Prompt: %s", str);
        ESP_LOGI(TAG, "Response modalities: TEXT, IMAGE");
        ESP_LOGI(TAG, "Aspect ratio: %s, Image size: %s", aspect_ratio_str,
                 model_supports_image_size(model) ? "1K" : "N/A (not supported)");
        ESP_LOGI(TAG, "Request body length: %d bytes", strlen(request_body));
    } else {
        is_success = false;
        ESP_LOGE(TAG, "Failed to serialize request JSON");
    }
}

char *gemini_image_bsp::get_ImgName() {
    if (!is_success) {
        ESP_LOGE(TAG, "set_Chat was not called or failed");
        return NULL;
    }

    const char *result = gemini_generate_image();
    if (result == NULL) {
        ESP_LOGE(TAG, "Image generation failed");
        return NULL;
    }

    return sdcard_path;
}

char *gemini_image_bsp::get_ImgName_Direct(bool direct_display) {
    if (!is_success) {
        ESP_LOGE(TAG, "set_Chat was not called or failed");
        return NULL;
    }

    // Pass direct_display to skip SD card save when true
    const char *result = gemini_generate_image(direct_display);
    if (result == NULL) {
        ESP_LOGE(TAG, "Image generation failed");
        return NULL;
    }

    if (direct_display) {
        // Return special marker, buffer is ready for direct display
        snprintf(sdcard_path, 98, "__DIRECT__");
        ESP_LOGI(TAG, "Direct display mode: buffer ready (%dx%d)",
                 _last_target_w, _last_target_h);
    }

    return sdcard_path;
}
