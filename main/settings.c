#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "atarixl"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = XL_MODEL_800XL;
    settings.tv = XL_TV_PAL;
    settings.basic = 1;
    settings.joystick = 1;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 < XL_MODEL_COUNT) {
        settings.model = u8;
    }
    if (nvs_get_u8(h, "tv", &u8) == ESP_OK && u8 <= XL_TV_NTSC) {
        settings.tv = u8;
    }
    if (nvs_get_u8(h, "basic", &u8) == ESP_OK && u8 <= 1) {
        settings.basic = u8;
    }
    if (nvs_get_u8(h, "joystick", &u8) == ESP_OK && (u8 == 1 || u8 == 2)) {
        settings.joystick = u8;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "tv", settings.tv);
    nvs_set_u8(h, "basic", settings.basic);
    nvs_set_u8(h, "joystick", settings.joystick);
    nvs_commit(h);
    nvs_close(h);
}
