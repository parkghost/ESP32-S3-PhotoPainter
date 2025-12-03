#include "axp_prot.h"
#include "button_bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "i2c_bsp.h"
#include "led_bsp.h"
#include "sdcard_bsp.h"
#include "user_app.h"
#include <cmath>
#include <stdio.h>
#include <string.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "epaper_port.h"

#include "client_bsp.h"
#include "json_data.h"

#include "esp32_ai_bsp.h"
#include "gemini_image_bsp.h"

#include "application.h"

// Abstract base class pointer for AI image generation
static floyd_steinberg *dev_ai_base = NULL;
static ai_provider_t current_provider = AI_PROVIDER_VOLCANO;

// Typed pointers for each provider
static esp32_ai_bsp *dev_ai_volcano = NULL;
static gemini_image_bsp *dev_ai_gemini = NULL;

static uint8_t *epd_blackImage = NULL; 
static uint32_t Imagesize;             

i2c_equipment_shtc3 *dev_shtc3 = NULL;
json_data_t *json_data = NULL; 
char THData[40];

int                sdcard_bmp_Quantity = 0; // The number of images in the sdcard directory  // Used in Xiaozhi main code
int                sdcard_doc_count    = 0; // The index of the image  // Used in Xiaozhi main code
int                is_ai_img           = 1; // If the current process is refreshing, then the AI-generated images cannot be generated
EventGroupHandle_t ai_IMG_Group;            // Task group for ai_IMG
EventGroupHandle_t ai_IMG_Score_Group;      // Task group for polling and playing high-score images by AI in ai_IMG
static bool        g_ai_direct_display = true; // AI image direct display mode (skip SD card I/O)

char   *str_ai_chat_buff = NULL; // This is a text-to-image conversion. The default text length is 1024.
#define STR_AI_CHAT_BUFF_SIZE 1024
int     IMG_Score        = 0;    // Score the image
gemini_aspect_ratio_t ai_img_aspect_ratio = ASPECT_RATIO_16_9;  // Default to landscape (16:9)
scale_mode_t ai_img_scale_mode = SCALE_MODE_FILL;  // Default to fill (crop excess)
list_t *sdcard_score     = NULL; // The high-score list requires memory allocation and deallocation
char    score_name[100];         // Poll the current image

char sleep_buff[64]; 

SemaphoreHandle_t ai_img_while_semap; 

void xiaozhi_init_received(const char *arg1)
{
    static uint8_t one_time = 0;
    if (one_time)
        return;
    if (strstr(arg1, "版本") != NULL) {
        one_time = 1;
        // Weather query disabled - not available outside China mainland
        // const char *str = auto_get_weather_json();
        // json_data = json_read_data(str);
        json_data = NULL;
        ESP_LOGI("xiaozhi", "Weather query disabled, skipping weather display");
        xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(0));
        // Skip weather display on EPD - don't set epaper_groups bit 0
        // xEventGroupSetBits(epaper_groups, set_bit_button(0));
    }
}

void xiaozhi_application_received(const char *str) {
    static bool is_led_flag = false;
    //ESP_LOGE("adasd", "%s", str);
    strncpy(sleep_buff, str, sizeof(sleep_buff) - 1);
    sleep_buff[sizeof(sleep_buff) - 1] = '\0';
    if (is_led_flag) {
        if (strstr(sleep_buff, "idle") != NULL) {
            gpio_set_level((gpio_num_t) 45, 1);
            is_led_flag = false;
        }
    } else {
        if ((strstr(sleep_buff, "listening") != NULL) || (strstr(sleep_buff, "speaking") != NULL)) {
            gpio_set_level((gpio_num_t) 45, 0);
            is_led_flag = true;
        }
    }
}

void xiaozhi_ai_Message(const char *arg1, const char *arg2) //ai chat
{
    if (!strcmp(arg1, "user")) {
        strncpy(str_ai_chat_buff, arg2, STR_AI_CHAT_BUFF_SIZE - 1);
        str_ai_chat_buff[STR_AI_CHAT_BUFF_SIZE - 1] = '\0';
    }
}

static void gui_user_Task(void *arg) {
    int *sdcard_doc = (int *) arg; 
    Imagesize       = ((EXAMPLE_LCD_WIDTH % 2 == 0) ? (EXAMPLE_LCD_WIDTH / 2) : (EXAMPLE_LCD_WIDTH / 2 + 1)) * EXAMPLE_LCD_HEIGHT;
    epd_blackImage  = (uint8_t *) heap_caps_malloc(Imagesize * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(epd_blackImage);
    /*刷图的公共部分*/
    Paint_NewImage(epd_blackImage, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SetRotate(180);
    Paint_SelectImage(epd_blackImage); 
    Paint_Clear(EPD_7IN3E_WHITE);      
    /**/
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(epaper_groups, set_bit_all, pdTRUE, pdFALSE, portMAX_DELAY); 
        if (pdTRUE == xSemaphoreTake(epaper_gui_semapHandle, 2000))                                         
        {
            
            xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(6));
            Green_led_arg = 1;
            is_ai_img     = 0;           
            if (get_bit_button(even, 0)) 
            {
                vTaskDelay(pdMS_TO_TICKS(3000));  
                char      *strURL = NULL;
                json_aqi_t aqi_data;
                uint16_t   x_or = 0;
                GUI_ReadBmp_RGB_6Color("/sdcard/01_sys_init_img/00_init.bmp", 0, 0);
                strURL = getSdCardImageDirectory(json_data->td_type);
                GUI_ReadBmp_RGB_6Color(strURL, 86, 92);
                strURL = getSdCardImageDirectory(json_data->tmr_type);
                GUI_ReadBmp_RGB_6Color(strURL, 274, 92);
                strURL = getSdCardImageDirectory(json_data->tdat_type);
                GUI_ReadBmp_RGB_6Color(strURL, 462, 92);
                strURL = getSdCardImageDirectory(json_data->stdat_type);
                GUI_ReadBmp_RGB_6Color(strURL, 650, 92);
                
                Paint_DrawString_CN(82, 34, json_data->td_weather, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(84, 58, json_data->td_week, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(74, 176, json_data->td_Temp, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(74, json_data->td_type);
                Paint_DrawString_CN(x_or, 208, json_data->td_type, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(74, json_data->td_fx);
                Paint_DrawString_CN(x_or, 234, json_data->td_fx, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                aqi_data = getWeatherAQI(json_data->td_aqi);
                x_or     = reassignCoordinates(74, aqi_data.str);
                Paint_DrawString_CN(x_or, 264, aqi_data.str, &Font14CN, EPD_7IN3E_WHITE, aqi_data.color);
                
                Paint_DrawString_CN(270, 34, json_data->tmr_weather, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(272, 58, json_data->tmr_week, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(262, 176, json_data->tmr_Temp, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(262, json_data->tmr_type);
                Paint_DrawString_CN(x_or, 208, json_data->tmr_type, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(262, json_data->tmr_fx);
                Paint_DrawString_CN(x_or, 234, json_data->tmr_fx, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                aqi_data = getWeatherAQI(json_data->tmr_aqi);
                x_or     = reassignCoordinates(262, aqi_data.str);
                Paint_DrawString_CN(x_or, 264, aqi_data.str, &Font14CN, EPD_7IN3E_WHITE, aqi_data.color);
                
                Paint_DrawString_CN(458, 34, json_data->tdat_weather, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(460, 58, json_data->tdat_week, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(450, 176, json_data->tdat_Temp, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(450, json_data->tdat_type);
                Paint_DrawString_CN(x_or, 208, json_data->tdat_type, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(450, json_data->tdat_fx);
                Paint_DrawString_CN(x_or, 234, json_data->tdat_fx, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                aqi_data = getWeatherAQI(json_data->tdat_aqi);
                x_or     = reassignCoordinates(450, aqi_data.str);
                Paint_DrawString_CN(x_or, 264, aqi_data.str, &Font14CN, EPD_7IN3E_WHITE, aqi_data.color);
                
                Paint_DrawString_CN(646, 34, json_data->stdat_weather, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(648, 58, json_data->stdat_week, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(638, 176, json_data->stdat_Temp, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(638, json_data->stdat_type);
                Paint_DrawString_CN(x_or, 208, json_data->stdat_type, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                x_or = reassignCoordinates(638, json_data->stdat_fx);
                Paint_DrawString_CN(x_or, 234, json_data->stdat_fx, &Font14CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                aqi_data = getWeatherAQI(json_data->stdat_aqi);
                x_or     = reassignCoordinates(638, aqi_data.str);
                Paint_DrawString_CN(x_or, 264, aqi_data.str, &Font14CN, EPD_7IN3E_WHITE, aqi_data.color);

                Paint_DrawString_CN(44, 367, json_data->calendar, &Font22CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
                Paint_DrawString_CN(118, 410, json_data->td_week, &Font18CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);

                epaper_port_display(epd_blackImage); 
                
                heap_caps_free(json_data);
            } else if (get_bit_button(even, 1)) { 
                *sdcard_doc -= 1;
                list_node_t *sdcard_node = list_at(sdcard_scan_listhandle, *sdcard_doc); 
                if (sdcard_node != NULL)                                                 
                {
                    sdcard_node_t *sdcard_Name_node = (sdcard_node_t *) sdcard_node->val;
                    set_Currently_node(sdcard_node);
                    GUI_ReadBmp_RGB_6Color(sdcard_Name_node->sdcard_name, 0, 0);
                    epaper_port_display(epd_blackImage); 
                }
            } else if (get_bit_button(even, 2)) {
                ESP_LOGI("epaper_showTask", "Received AI image display event");
                list_node_t *node = list_at(sdcard_scan_listhandle, -1);
                if (node != NULL) {
                    sdcard_node_t *sdcard_Name_node_ai = (sdcard_node_t *) node->val;
                    set_Currently_node(node);

                    // Step 4: Read BMP from SD card - timing
                    int64_t bmp_read_start = esp_timer_get_time();
                    ESP_LOGI("epaper_showTask", "Loading BMP: %s", sdcard_Name_node_ai->sdcard_name);
                    GUI_ReadBmp_RGB_6Color(sdcard_Name_node_ai->sdcard_name, 0, 0);
                    int64_t bmp_read_end = esp_timer_get_time();
                    int64_t bmp_read_ms = (bmp_read_end - bmp_read_start) / 1000;
                    ESP_LOGI("epaper_showTask", "[TIMING] BMP read from SD card: %lld ms", bmp_read_ms);

                    // Step 5: E-paper display refresh - timing
                    int64_t epaper_start = esp_timer_get_time();
                    ESP_LOGI("epaper_showTask", "Starting e-paper refresh...");
                    epaper_port_display(epd_blackImage);
                    int64_t epaper_end = esp_timer_get_time();
                    int64_t epaper_ms = (epaper_end - epaper_start) / 1000;
                    ESP_LOGI("epaper_showTask", "[TIMING] E-paper refresh: %lld ms (%.1f seconds)", epaper_ms, epaper_ms / 1000.0f);

                    // Summary
                    ESP_LOGI("epaper_showTask", "╔════════════════════════════════════════╗");
                    ESP_LOGI("epaper_showTask", "║     DISPLAY TIMING SUMMARY             ║");
                    ESP_LOGI("epaper_showTask", "╠════════════════════════════════════════╣");
                    ESP_LOGI("epaper_showTask", "║ BMP Read:      %6lld ms              ║", bmp_read_ms);
                    ESP_LOGI("epaper_showTask", "║ E-Paper:       %6lld ms (%5.1f s)    ║", epaper_ms, epaper_ms / 1000.0f);
                    ESP_LOGI("epaper_showTask", "║ Total:         %6lld ms (%5.1f s)    ║", bmp_read_ms + epaper_ms, (bmp_read_ms + epaper_ms) / 1000.0f);
                    ESP_LOGI("epaper_showTask", "╚════════════════════════════════════════╝");
                } else {
                    ESP_LOGE("epaper_showTask", "No node found in list");
                }
            } else if (get_bit_button(even, 3)) {
                GUI_ReadBmp_RGB_6Color(score_name, 0, 0);
                epaper_port_display(epd_blackImage);
            } else if (get_bit_button(even, 4)) {
                // Direct display from buffer (skip SD card I/O)
                ESP_LOGI("epaper_showTask", "Received direct buffer display event");

                if (dev_ai_gemini != NULL) {
                    uint8_t *buffer = dev_ai_gemini->get_DitheredBuffer();
                    int img_w = dev_ai_gemini->get_TargetWidth();
                    int img_h = dev_ai_gemini->get_TargetHeight();

                    if (buffer != NULL && img_w > 0 && img_h > 0) {
                        // Direct draw from buffer - timing
                        int64_t draw_start = esp_timer_get_time();
                        ESP_LOGI("epaper_showTask", "Direct buffer draw: %dx%d", img_w, img_h);
                        GUI_DirectDisplay_RGB888_6Color(buffer, img_w, img_h, 0, 0);
                        int64_t draw_ms = (esp_timer_get_time() - draw_start) / 1000;
                        ESP_LOGI("epaper_showTask", "[TIMING] Direct buffer draw: %lld ms", draw_ms);

                        // E-paper display refresh - timing
                        int64_t epaper_start = esp_timer_get_time();
                        ESP_LOGI("epaper_showTask", "Starting e-paper refresh...");
                        epaper_port_display(epd_blackImage);
                        int64_t epaper_ms = (esp_timer_get_time() - epaper_start) / 1000;
                        ESP_LOGI("epaper_showTask", "[TIMING] E-paper refresh: %lld ms", epaper_ms);

                        // Summary
                        ESP_LOGI("epaper_showTask", "╔════════════════════════════════════════╗");
                        ESP_LOGI("epaper_showTask", "║  DIRECT DISPLAY TIMING SUMMARY         ║");
                        ESP_LOGI("epaper_showTask", "╠════════════════════════════════════════╣");
                        ESP_LOGI("epaper_showTask", "║ Buffer Draw:   %6lld ms (SD skipped) ║", draw_ms);
                        ESP_LOGI("epaper_showTask", "║ E-Paper:       %6lld ms (%5.1f s)    ║", epaper_ms, epaper_ms / 1000.0f);
                        ESP_LOGI("epaper_showTask", "║ Total:         %6lld ms (%5.1f s)    ║", draw_ms + epaper_ms, (draw_ms + epaper_ms) / 1000.0f);
                        ESP_LOGI("epaper_showTask", "╚════════════════════════════════════════╝");
                    } else {
                        ESP_LOGE("epaper_showTask", "Direct buffer is NULL or invalid size");
                    }
                } else {
                    ESP_LOGE("epaper_showTask", "Gemini provider not initialized");
                }
            }
            xSemaphoreGive(epaper_gui_semapHandle);
            Green_led_arg = 0;
            is_ai_img     = 1;
            ESP_LOGI("epaper_showTask", "Display complete, is_ai_img reset to 1, ready for next request");
        }
    }
}

static void ai_IMG_Task(void *arg) {
    char *chatStr = (char *) arg;
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(ai_IMG_Group, (0x01) | (0x02) | (0x04) | (0x08), pdTRUE, pdFALSE, portMAX_DELAY);

        if (get_bit_button(even, 0)) {
            ESP_LOGE("chat", "%s", chatStr);
            char *str = NULL;

            // Use the appropriate provider based on configuration
            if (current_provider == AI_PROVIDER_GEMINI && dev_ai_gemini != NULL) {
                dev_ai_gemini->set_AspectRatio(ai_img_aspect_ratio);
                dev_ai_gemini->set_ScaleMode(ai_img_scale_mode);
                dev_ai_gemini->set_Chat(chatStr);
                // Use direct display mode if configured
                str = dev_ai_gemini->get_ImgName_Direct(g_ai_direct_display);
            } else if (dev_ai_volcano != NULL) {
                dev_ai_volcano->set_Chat(chatStr);
                str = dev_ai_volcano->get_ImgName();
            } else {
                ESP_LOGE("ai_IMG_Task", "No AI provider available (both gemini and volcano are NULL)");
            }

            if (str != NULL) {
                ESP_LOGI("ai_IMG_Task", "Image generation success, path: %s", str);

                // Check if direct display mode
                if (g_ai_direct_display && strcmp(str, "__DIRECT__") == 0) {
                    // Direct display mode - trigger new event bit 4
                    ESP_LOGI("ai_IMG_Task", "Triggering direct display (skip SD card)...");
                    xEventGroupSetBits(epaper_groups, set_bit_button(4));
                } else {
                    // SD card mode - existing flow
                    sdcard_node_t *sdcard_node_data = (sdcard_node_t *) malloc(sizeof(sdcard_node_t));
                    assert(sdcard_node_data);
                    strcpy(sdcard_node_data->sdcard_name, str);
                    sdcard_node_data->name_score = 1;
                    list_rpush(sdcard_scan_listhandle, list_node_new(sdcard_node_data));
                    ESP_LOGI("ai_IMG_Task", "Triggering epaper display (from SD card)...");
                    xEventGroupSetBits(epaper_groups, set_bit_button(2));
                }
            } else {
                ESP_LOGE("ai_IMG_Task", "Image generation failed, str is NULL");
            }
            ESP_LOGI("ai_IMG_Task", "Image task complete, waiting for next event");
        } else if (get_bit_button(even, 1)) {
            sdcard_bmp_Quantity = list_iterator(); 
            xSemaphoreGive(ai_img_while_semap);    
        } else if (get_bit_button(even, 2)) {
            list_node_t   *node               = get_Currently_node();
            sdcard_node_t *sdcard_curren_node = (sdcard_node_t *) node->val;
            sdcard_curren_node->name_score    = IMG_Score; 
            xSemaphoreGive(ai_img_while_semap);            
        } else if (get_bit_button(even, 3)) {              
            auto &app = Application::GetInstance();
            if (strstr(sleep_buff, "idle") != NULL) 
            {

            } else if (strstr(sleep_buff, "listening") != NULL) 
            {
                app.ToggleChatState();
            } else if (strstr(sleep_buff, "speaking") != NULL) {
                app.ToggleChatState();
                vTaskDelay(pdMS_TO_TICKS(500));
                app.ToggleChatState();
            }
        }
    }
    vTaskDelete(NULL);
}

static int list_score_iterator(list_t *list_data, list_t *list_out_score) 
{
    if (list_out_score == NULL || list_data == NULL) {
        ESP_LOGE("list", "list out fill");
        return -1;
    }
    int              value = 0;
    list_iterator_t *it    = list_iterator_new(list_data, LIST_HEAD); 
    list_node_t     *node  = list_iterator_next(it);
    while (node != NULL) {
        sdcard_node_t *sdcard_node = (sdcard_node_t *) node->val;
        if (sdcard_node->name_score >= 3) {
            char *score = (char *) malloc(100);
            strcpy(score, sdcard_node->sdcard_name);
            list_rpush(list_out_score, list_node_new(score)); 
            value++;
        }
        node = list_iterator_next(it);
    }
    return value;
}

void ai_Score_Task(void *arg) 
{
    int name_value = 0;
    int _ats       = 0;
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(ai_IMG_Score_Group, (0x01) | (0x02), pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 0)) {
            if (sdcard_score == NULL) {
                sdcard_score = list_new();                                                
                name_value   = list_score_iterator(sdcard_scan_listhandle, sdcard_score); 
                //ESP_LOGE("OK", "OK1");
            }
            if (sdcard_score != NULL) {
                if (name_value > 0) {
                    list_node_t *sdcard_node = list_at(sdcard_score, _ats); 
                    strcpy(score_name, (char *) sdcard_node->val);          
                    xEventGroupSetBits(epaper_groups, set_bit_button(3));   
                    //ESP_LOGE("OK", "OK2");
                    _ats++;
                    if (_ats == name_value) {
                        _ats = 0;
                    }
                }
            }
        } else if (get_bit_button(even, 1)) { 
            
            list_destroy(sdcard_score);
            sdcard_score = NULL;
            xEventGroupClearBits(ai_IMG_Score_Group, 0x02);
            //ESP_LOGE("OK", "OK2");
        }
        vTaskDelay(pdMS_TO_TICKS(1000 * 60 * 30)); 
    }
}

void key_wakeUp_user_Task(void *arg) {
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(key_groups, (0x01), pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (even & 0x01) {
            gpio_set_level((gpio_num_t) 45, 0);
            std::string wake_word = "你好小智";
            Application::GetInstance().WakeWordInvoke(wake_word);
        }
    }
}

void pwr_sleep_user_Task(void *arg) {
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(pwr_groups, (0x01), pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (even & 0x01) {
            xEventGroupSetBits(ai_IMG_Group, 0x08);
            gpio_set_level((gpio_num_t) 45, 1); 
        }
    }
}

char* Get_TemperatureHumidity(void) {
    shtc3_data_t data = dev_shtc3->readTempHumi();
    if(!data.RH || !data.Temp)
    return NULL;
    snprintf(THData,40,"温度:%.2f,湿度:%.2f",data.Temp,data.RH);
    return THData;
}

void User_xiaozhi_app_init(void)                     // Initialization in the Xiaozhi mode
{
    gpio_set_level((gpio_num_t) 45, 0);
    dev_shtc3 = new i2c_equipment_shtc3();
    ai_img_while_semap = xSemaphoreCreateBinary();
    str_ai_chat_buff   = (char *) heap_caps_malloc(STR_AI_CHAT_BUFF_SIZE, MALLOC_CAP_SPIRAM);
    ai_IMG_Group       = xEventGroupCreate();
    ai_IMG_Score_Group = xEventGroupCreate();
    ai_model_t *ai_model_data = json_sdcard_txt_aimodel();
    if (ai_model_data != NULL) {                      //Obtain key, url, model
        ESP_LOGI("ai_model", "model:%s,key:%s,url:%s,provider:%d",
                 ai_model_data->model, ai_model_data->key, ai_model_data->url, ai_model_data->provider);
    } else {
        return;
    }

    // Initialize the appropriate AI provider based on configuration
    current_provider = ai_model_data->provider;
    g_ai_direct_display = ai_model_data->ai_direct_display;
    ESP_LOGI("ai_model", "AI direct display mode: %s", g_ai_direct_display ? "enabled" : "disabled");

    if (current_provider == AI_PROVIDER_GEMINI) {
        ESP_LOGI("ai_model", "Initializing Gemini image provider");
        dev_ai_gemini = new gemini_image_bsp(ai_model_data->model, ai_model_data->key, 800, 480);
        dev_ai_base = dev_ai_gemini;
    } else {
        ESP_LOGI("ai_model", "Initializing Volcano Engine image provider");
        dev_ai_volcano = new esp32_ai_bsp(ai_model_data->model, ai_model_data->url, ai_model_data->key, 800, 480);
        dev_ai_base = dev_ai_volcano;
    }

    // Verify AI provider was initialized successfully
    if (dev_ai_base == NULL) {
        ESP_LOGE("ai_model", "Failed to initialize AI provider, aborting");
        return;
    }

    //if(ai_model_data != NULL) {free(ai_model_data);ai_model_data = NULL;}
    list_scan_dir("/sdcard/05_user_ai_img"); // Place the image data under the linked list
    sdcard_bmp_Quantity = list_iterator();   // Traverse the linked list to count the number of images
    xTaskCreate(gui_user_Task, "gui_user_Task", 6 * 1024, &sdcard_doc_count, 2, NULL);
    xTaskCreate(ai_IMG_Task, "ai_IMG_Task", 6 * 1024, str_ai_chat_buff, 2, NULL);
    xTaskCreate(ai_Score_Task, "ai_Score_Task", 4 * 1024, NULL, 2, NULL);
    xTaskCreate(key_wakeUp_user_Task, "key_wakeUp_user_Task", 4 * 1024, NULL, 3, NULL); 
    xTaskCreate(pwr_sleep_user_Task, "pwr_sleep_user_Task", 4 * 1024, NULL, 3, NULL);   
}
