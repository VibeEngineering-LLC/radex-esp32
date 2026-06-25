// ══════════════════════════════════════════════════════════════════════
//  radex_read_hook.h — BLEClientNode для перехвата READ_CHAR_EVT
//
//  Назначение: Radex MR107ion использует READ-poll, не Notify. ESPHome
//  штатной платформы для periodic-read нет, поэтому делаем hook через
//  BLEClientBase::register_ble_node() и сами парсим payload по handle.
//
//  Паттерн зафиксирован в скилле esp32-dev (BLEClientNode READ-hook
//  pattern, commit 9f5df52, 2026-06-12).
//
//  GATT-карта MR107ion FE651700 (полный профиль, 2026-06-13):
//    handle  имя            тип            описание
//    0x0040  OAR_sred       float32 LE     радон среднее, Бк/м³
//    0x0046  t_izm_last     uint32 LE      uptime прибора, сек
//    0x0049  OAR_last       float32 LE     радон последнее, Бк/м³
//    0x0052  OAR_min        float32 LE     радон минимум, Бк/м³
//    0x0055  OAR_max        float32 LE     радон максимум, Бк/м³
//    0x0058  temper_x10     int16 LE *)    температура × 10 → °C
//    0x005E  humidity       uint8          влажность, %
//
//  *) В btsnoop'ах с прибора 0214 наблюдались только положительные сэмплы
//     температуры, отличить uint16 от int16 two's complement по этим
//     данным нельзя (для значений 0..32767 битовое представление совпадает).
//     Декодируем как int16 LE — это safe super-set: значения 0..+3276.7 °C
//     декодируются одинаково корректно при любой из двух интерпретаций,
//     а отрицательная температура (если прибор её отдаёт) проходит через
//     guard −40..+85 °C корректно. uint16-декодер на отрицательном сэмпле
//     выдал бы ~6500 °C, и guard t<=85 отсёк бы кадр.
//
//  Полный 15-char профиль — в скилле radex-ble/references/mr107ion.md.
// ══════════════════════════════════════════════════════════════════════
#pragma once

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <esp_gattc_api.h>
#include <esp_heap_caps.h>  // v0.2.1-diag: heap_caps_* для heap-лога в interval-лямбде
#include <cstring>

namespace esphome {
namespace radex_hook {

// Forward callback — устанавливается из YAML lambda при on_boot
using radex_value_cb_t = void(*)(uint16_t handle, const uint8_t* data, uint16_t len);

class RadexReadHook : public ble_client::BLEClientNode {
 public:
  void set_callback(radex_value_cb_t cb) { this->cb_ = cb; }
  uint32_t get_read_count() const { return this->read_count_; }
  uint32_t get_err_count() const { return this->err_count_; }

  void gattc_event_handler(esp_gattc_cb_event_t event,
                           esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t* param) override {
    if (event != ESP_GATTC_READ_CHAR_EVT) return;
    if (param->read.status != ESP_GATT_OK) {
      this->err_count_++;
      ESP_LOGW("radex_hook", "READ h=0x%04X status=%d (err#%u)",
               param->read.handle, param->read.status, this->err_count_);
      return;
    }
    this->read_count_++;
    if (this->cb_) {
      this->cb_(param->read.handle, param->read.value, param->read.value_len);
    } else {
      // Debug fallback: hex-dump если callback не зарегистрирован
      char hex[64]; size_t pos = 0;
      for (size_t i = 0; i < param->read.value_len && pos < 60; i++)
        pos += snprintf(hex + pos, 64 - pos, "%02X ", param->read.value[i]);
      ESP_LOGI("radex_hook", "h=0x%04X len=%u %s",
               param->read.handle, param->read.value_len, hex);
    }
  }

 private:
  radex_value_cb_t cb_ = nullptr;
  uint32_t read_count_ = 0;
  uint32_t err_count_ = 0;
};

// Утилиты декодирования (LE)
inline float decode_float_le(const uint8_t* d, uint16_t len) {
  if (len < 4) return NAN;
  float v;
  std::memcpy(&v, d, 4);
  return v;
}
// Signed 16-bit LE — super-set safe для temper_x10 (см. GATT-карту выше).
// Возвращает true при успехе. Caller обязан проверять результат до
// использования out (callsites в YAML делают `if (len >= 2)` отдельно для
// раннего скипа без чтения).
inline int16_t decode_i16_le(const uint8_t* d, uint16_t len) {
  if (len < 2) return 0;
  int16_t v;
  std::memcpy(&v, d, 2);
  return v;
}

// Раньше здесь были decode_u32_le / decode_u16_le / read_next_handle
// (helper для interval-loop). Все три больше не используются в рабочих
// YAML — round-robin READ инлайнится прямо в interval-лямбде через
// esp_ble_gattc_read_char(), а u32/u16 декодеры не нужны (все handle
// либо float32 / int16 / uint8). Удалены в audit-fix 2026-06-25 (F5a/b).
// При необходимости — восстановить из git-истории.

}  // namespace radex_hook
}  // namespace esphome