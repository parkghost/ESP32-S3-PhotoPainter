#ifndef GEMINI_IMAGE_BSP_H
#define GEMINI_IMAGE_BSP_H

#include "ArduinoJson-v7.4.1.h"
#include "esp_http_client.h"
#include "floyd_steinberg.h"

// HTTP response callback structure
typedef struct {
    char *buffer;
    int buffer_len;
} gemini_http_response_t;

// Aspect ratio options for image generation
typedef enum {
    ASPECT_RATIO_16_9,  // Landscape (800x480)
    ASPECT_RATIO_9_16   // Portrait (480x800)
} gemini_aspect_ratio_t;

// Scale mode options for image scaling
typedef enum {
    SCALE_MODE_FILL,  // Fill target area, crop excess (default, no distortion)
    SCALE_MODE_FIT    // Fit entire image, pad with white (no distortion)
} scale_mode_t;

/**
 * Gemini Image Generation BSP
 * Uses Google Gemini API (gemini-2.5-flash-image) for image generation
 * Returns base64-encoded PNG images instead of URLs
 */
class gemini_image_bsp : public floyd_steinberg
{
private:
    JsonDocument doc;
    char *request_body = NULL;          // JSON request body buffer
    const char *api_key = NULL;         // Gemini API key
    const char *model = NULL;           // Model name (e.g., "gemini-2.5-flash-image")
    char sdcard_path[100] = {""};       // SD card file path for saved image
    int path_value = 0;                 // File index counter
    bool is_success = false;            // Flag for successful request setup
    uint8_t *png_buffer = NULL;         // Store decoded PNG data
    uint8_t *floyd_buffer = NULL;       // Store Floyd-Steinberg dithered data
    int _width;
    int _height;
    gemini_aspect_ratio_t _aspect_ratio;  // Current aspect ratio setting
    scale_mode_t _scale_mode;             // Current scale mode setting
    int _last_target_w = 0;               // Last generated image width (for direct display)
    int _last_target_h = 0;               // Last generated image height (for direct display)

    // Base64 decoding table
    static const int8_t base64_decode_table[256];

    static int _http_event_handler(esp_http_client_event_t *evt);

    // Decode base64 string to binary data
    int base64_decode(const char *input, size_t input_len, uint8_t *output, size_t *output_len);

    // Call Gemini API and get base64-encoded image
    const char* gemini_generate_image(bool skip_sd_save = false);

    // Decode PNG to RGB888 format
    uint8_t png_to_rgb888(uint8_t *png_data, int png_len, uint8_t **rgb_buffer, int *rgb_len, int *width, int *height);

public:
    /**
     * Constructor
     * @param ai_model Model name (e.g., "gemini-2.5-flash-image")
     * @param gemini_api_key API key from Google AI Studio
     * @param width Image width (default 800)
     * @param height Image height (default 480)
     */
    gemini_image_bsp(const char *ai_model, const char *gemini_api_key, const int width, const int height);
    ~gemini_image_bsp();

    /**
     * Set the aspect ratio for image generation
     * @param ratio ASPECT_RATIO_16_9 for landscape, ASPECT_RATIO_9_16 for portrait
     */
    void set_AspectRatio(gemini_aspect_ratio_t ratio);

    /**
     * Get the current aspect ratio setting
     * @return Current aspect ratio
     */
    gemini_aspect_ratio_t get_AspectRatio() const;

    /**
     * Set the scale mode for image scaling
     * @param mode SCALE_MODE_FILL to crop excess, SCALE_MODE_FIT to pad with white
     */
    void set_ScaleMode(scale_mode_t mode);

    /**
     * Get the current scale mode setting
     * @return Current scale mode
     */
    scale_mode_t get_ScaleMode() const;

    /**
     * Set the prompt for image generation
     * @param str The prompt text describing the desired image
     */
    void set_Chat(const char *str);

    /**
     * Generate image and save to SD card
     * @return Path to the saved BMP file on SD card, or NULL on failure
     */
    char *get_ImgName();

    /**
     * Generate image with option to skip SD card save
     * @param direct_display If true, skips saving to SD card (for direct display)
     * @return Path string or "__DIRECT__" marker if direct_display=true
     */
    char *get_ImgName_Direct(bool direct_display);

    /**
     * Get pointer to the dithered buffer (RGB888, 6-color palette)
     * Valid after successful get_ImgName() or get_ImgName_Direct() call
     * @return Pointer to floyd_buffer, or NULL if not available
     */
    uint8_t* get_DitheredBuffer() { return floyd_buffer; }

    /**
     * Get the width of the last generated image
     * @return Width in pixels
     */
    int get_TargetWidth() const { return _last_target_w; }

    /**
     * Get the height of the last generated image
     * @return Height in pixels
     */
    int get_TargetHeight() const { return _last_target_h; }
};

#endif
