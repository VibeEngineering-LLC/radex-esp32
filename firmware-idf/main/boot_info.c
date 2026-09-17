// #RADEX-294: причина прошлой перезагрузки и сколько плата прожила до неё.
// NVS-пространство "boot": "reason" (esp_reset_reason этого старта),
// "up_s" (uptime, записанный не чаще раза в BOOT_SAVE_PERIOD_S).
#include "boot_info.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "boot_info";

// Период 600 с. Одна запись u32 — одна 32-байтная запись NVS; 144 записи в
// сутки. Раздел nvs 0x6000 = 6 страниц по 126 записей, страница заполняется
// менее чем за сутки, и каждый сектор стирается примерно раз в 5-6 суток:
// при ресурсе 100 000 циклов это сотни лет. Цена — точность prev_uptime_s:
// занижение не больше 10 минут.
#define BOOT_SAVE_PERIOD_S 600

static int s_reason = -1;
static uint32_t s_prev_uptime_s;
static int64_t s_last_save_s;

void boot_info_init(void)
{
    s_reason = (int)esp_reset_reason();
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS boot не открылся");
        return;
    }
    uint32_t up = 0;
    if (nvs_get_u32(h, "up_s", &up) == ESP_OK) s_prev_uptime_s = up;
    nvs_set_i32(h, "reason", s_reason);
    nvs_set_u32(h, "up_s", 0);   // 0 после старта: не дожил до первой записи
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "причина старта %d, прошлый uptime не меньше %u с",
             s_reason, (unsigned)s_prev_uptime_s);
}

void boot_info_tick(void)
{
    int64_t up = esp_timer_get_time() / 1000000;
    if (up - s_last_save_s < BOOT_SAVE_PERIOD_S) return;
    s_last_save_s = up;
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "up_s", (uint32_t)up);
    nvs_commit(h);
    nvs_close(h);
}

int boot_info_last_reset_reason(void)
{
    return s_reason;
}

uint32_t boot_info_prev_uptime_s(void)
{
    return s_prev_uptime_s;
}
