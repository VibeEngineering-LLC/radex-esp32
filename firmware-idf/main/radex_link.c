// #USB-1 универсальная сборка: диспетчер канала связи с прибором. Реализует внешние функции
// ble_radex.h / ble_radex_journal.h (их зовут web_server.c, radex_data.c, main.c, ha_mqtt.c,
// target_switch.c — выше шва ничего не меняется) и направляет вызовы в BLE (bleimpl_*) или USB (usbimpl_*).
// Правило: прибор перечислен на USB-хосте -> USB, BLE на паузе; USB нет -> BLE, как в v1.12.0 (если
// Bluetooth не выключен в настройках); иначе канала нет. Переходы на лету, без перезагрузки: BLE-стек не
// гасится, а ставится на паузу (ble_radex_pause): поднят, но не сканирует и не подключается.
#include "radex_link.h"
#include "radex_link_impl.h"
#include "ble_radex.h"
#include "ble_radex_journal.h"

#include <nvs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "radex_link";

#define LINK_NVS_NS            "radex"   // то же пространство, что у выбора прибора (ble_radex.c)
#define LINK_NVS_BT            "bt_on"
#define LINK_BOOT_USB_WAIT_MS  3000      // на старте ждём перечисления USB, чтобы не начинать BLE зря
#define LINK_TICK_MS           500
#define LINK_USB_LOST_HOLD_MS  10000     // USB пропал: BLE возобновляется не сразу — переоткрытие порта
                                         // после 3 ошибок опроса не должно дёргать BLE туда-обратно
#define LINK_BLE_TASK_STACK    4096      // bleimpl_start жил в app_main (стек 3584)

static ble_radex_cb_t             s_cb;
static volatile bool              s_bt_enabled = true;
static volatile bool              s_ble_started;
static bool                       s_ble_paused;
static volatile radex_transport_t s_active = RADEX_TRANSPORT_NONE;
static int64_t                    s_usb_lost_us;

static bool bt_load(void)
{
    nvs_handle_t h;
    uint8_t v = 1;                           // по умолчанию Bluetooth включён
    if (nvs_open(LINK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, LINK_NVS_BT, &v);
        nvs_close(h);
    }
    return v != 0;
}

static bool bt_save(bool on)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, LINK_NVS_BT, on ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// bleimpl_start управление не возвращает (свой вечный цикл опроса) — поэтому отдельная задача.
static void ble_task(void *arg)
{
    (void)arg;
    bleimpl_start(s_cb);
    ESP_LOGE(TAG, "BLE-клиент не запустился");   // сюда — только при ошибке инициализации стека
    vTaskDelete(NULL);
}

static const char *tname(radex_transport_t t)
{
    return t == RADEX_TRANSPORT_USB ? "usb" : t == RADEX_TRANSPORT_BLE ? "ble" : "none";
}

static radex_transport_t decide(void)
{
    if (usbimpl_connected()) { s_usb_lost_us = 0; return RADEX_TRANSPORT_USB; }
    if (s_active == RADEX_TRANSPORT_USB) {
        int64_t now = esp_timer_get_time();
        if (s_usb_lost_us == 0) s_usb_lost_us = now;
        if (now - s_usb_lost_us < (int64_t)LINK_USB_LOST_HOLD_MS * 1000) return RADEX_TRANSPORT_USB;
    }
    return s_bt_enabled ? RADEX_TRANSPORT_BLE : RADEX_TRANSPORT_NONE;
}

// Вызывается только из задачи radex_link (и один раз из ble_radex_start до её создания).
static void apply(radex_transport_t next)
{
    bool pause = (next != RADEX_TRANSPORT_BLE);
    if (!pause && !s_ble_started) {
        s_ble_started = true;
        s_ble_paused = false;
        xTaskCreate(ble_task, "ble_main", LINK_BLE_TASK_STACK, NULL, 1, NULL);
    } else if (s_ble_started && pause != s_ble_paused) {
        s_ble_paused = pause;
        ble_radex_pause(pause);
    }
    if (next != s_active) {
        ESP_LOGW(TAG, "канал связи с прибором: %s -> %s", tname(s_active), tname(next));
        s_active = next;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    while (1) {
        apply(decide());
        vTaskDelay(pdMS_TO_TICKS(LINK_TICK_MS));
    }
}

// В отличие от v1.12.0 управление ВОЗВРАЩАЕТСЯ: BLE живёт в задаче ble_main, USB — в radex_usb.
void ble_radex_start(ble_radex_cb_t cb)
{
    s_cb = cb;
    s_bt_enabled = bt_load();
    ESP_LOGI(TAG, "Bluetooth в настройках: %s", s_bt_enabled ? "включён" : "выключен");
    usbimpl_start(cb);
    for (int t = 0; t < LINK_BOOT_USB_WAIT_MS && !usbimpl_connected(); t += 100) vTaskDelay(pdMS_TO_TICKS(100));
    apply(decide());
    xTaskCreate(link_task, "radex_link", 3072, NULL, 4, NULL);
}

// ── Внешние функции: выбор прибора и журнал — у активного канала; без канала отвечает USB-модуль
//    («прибор выбран», пустой список, журнал «usb: no device») — страница не уходит на вкладку поиска.
static bool via_usb(void) { return s_active != RADEX_TRANSPORT_BLE; }

bool ble_radex_has_target(void)                  { return via_usb() ? usbimpl_has_target() : bleimpl_has_target(); }
bool ble_radex_scanning(void)                    { return via_usb() ? usbimpl_scanning() : bleimpl_scanning(); }
int  ble_radex_found_json(char *buf, size_t len) { return via_usb() ? usbimpl_found_json(buf, len) : bleimpl_found_json(buf, len); }
bool ble_radex_set_target(const char *mac_str)   { return via_usb() ? usbimpl_set_target(mac_str) : bleimpl_set_target(mac_str); }
bool ble_radex_target_mac(char *buf, size_t len) { return via_usb() ? usbimpl_target_mac(buf, len) : bleimpl_target_mac(buf, len); }
void ble_radex_clear_target(void)                { if (via_usb()) usbimpl_clear_target(); else bleimpl_clear_target(); }

bool ble_radex_connected(void)
{
    if (s_active == RADEX_TRANSPORT_USB) return usbimpl_connected();
    if (s_active == RADEX_TRANSPORT_BLE) return bleimpl_connected();
    return false;
}

// Счётчики — сумма по обоим каналам: при переключении они не прыгают назад.
uint32_t ble_radex_reads_ok(void)    { return usbimpl_reads_ok() + bleimpl_reads_ok(); }
uint32_t ble_radex_read_errors(void) { return usbimpl_read_errors() + bleimpl_read_errors(); }
uint32_t ble_radex_disconnects(void) { return usbimpl_disconnects() + bleimpl_disconnects(); }
uint32_t ble_radex_open_fails(void)  { return usbimpl_open_fails() + bleimpl_open_fails(); }

void ble_radex_journal_init(void)                        { usbimpl_journal_init(); bleimpl_journal_init(); }
void ble_radex_journal_request(void)                     { if (via_usb()) usbimpl_journal_request(); else bleimpl_journal_request(); }
bool ble_radex_journal_busy(void)                        { return via_usb() ? usbimpl_journal_busy() : bleimpl_journal_busy(); }
bool ble_radex_journal_pending(void)                     { return via_usb() ? usbimpl_journal_pending() : bleimpl_journal_pending(); }
int  ble_radex_journal_json(char *buf, size_t len)       { return via_usb() ? usbimpl_journal_json(buf, len) : bleimpl_journal_json(buf, len); }
bool ble_radex_journal_get_result(radex_journal_t *out)  { return via_usb() ? usbimpl_journal_get_result(out) : bleimpl_journal_get_result(out); }

// ── Состояние и настройка для web_server.c / radex_data.c ─────────────────
radex_transport_t radex_link_transport(void) { return s_active; }
const char *radex_link_transport_str(void)   { return tname(s_active); }
bool radex_link_bt_enabled(void)             { return s_bt_enabled; }
bool radex_link_ble_stack_up(void)           { return s_ble_started && ble_radex_stack_up(); }

bool radex_link_set_bt_enabled(bool on)
{
    if (!bt_save(on)) { ESP_LOGE(TAG, "настройка Bluetooth не записана в NVS"); return false; }
    s_bt_enabled = on;
    ESP_LOGW(TAG, "Bluetooth %s в настройках", on ? "включён" : "выключен");
    return true;
}
