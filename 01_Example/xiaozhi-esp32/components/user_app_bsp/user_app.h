#ifndef USER_APP_H
#define USER_APP_H
#include "freertos/FreeRTOS.h"
#include "gemini_image_bsp.h"


uint8_t User_Mode_init(void);       // main.cc

extern EventGroupHandle_t
    Green_led_Mode_queue; 
extern EventGroupHandle_t
    Red_led_Mode_queue; 
extern SemaphoreHandle_t epaper_gui_semapHandle;
extern uint8_t Green_led_arg;           
extern uint8_t Red_led_arg;             
extern EventGroupHandle_t epaper_groups;


void User_xiaozhi_app_init(void); // init
void xiaozhi_init_received(const char *arg1);
void xiaozhi_ai_Message(const char *arg1, const char *arg2);
void xiaozhi_application_received(const char *str);
char* Get_TemperatureHumidity(void);
extern int sdcard_bmp_Quantity;
extern int sdcard_doc_count; 
extern int is_ai_img;        
extern EventGroupHandle_t ai_IMG_Group;
// extern int is_ai_buff_flag;
extern int IMG_Score; 
extern SemaphoreHandle_t
    ai_img_while_semap; 
extern EventGroupHandle_t ai_IMG_Score_Group;
extern char *str_ai_chat_buff;  // AI image generation prompt buffer (1024 bytes)
extern gemini_aspect_ratio_t ai_img_aspect_ratio;  // AI image aspect ratio (16:9 or 9:16)
extern scale_mode_t ai_img_scale_mode;  // AI image scale mode (fill or fit)


void User_Basic_mode_app_init(void);


void User_Network_mode_app_init(void);

void Mode_Selection_Init(void);

#endif