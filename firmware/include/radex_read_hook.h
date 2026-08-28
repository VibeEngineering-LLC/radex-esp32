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
#include <esp_gap_ble_api.h>
#include <esp_heap_caps.h>  // v0.2.1-diag: heap_caps_* для heap-лога в interval-лямбде
#include <cstring>

// #RADEX-4 H44 (2026-08-27): Bluedroid САМ запускает discovery на каждом
// соединении (bta_gattc_act.c, auto_disc=TRUE по умолчанию) — он никогда не
// завершается (H42), и наши READ вставали в очередь ПОЗАДИ него. Отключается
// недокументированным API («Add For BLE PTS» в исходнике ESP-IDF). Проверено:
// 705 READ / 0 разрывов за 900с на диагностике против разрывов 5-12с без
// этого вызова. Подробности: radex-ble-loop-run-2026-08-24.md, «ЛИНИЯ ЗАКРЫТА».
extern "C" uint8_t BTA_GATTC_AutoDiscoverEnable(uint8_t enable);

namespace esphome {
namespace radex_hook {

// Forward callback — устанавливается из YAML lambda при on_boot
using radex_value_cb_t = void(*)(uint16_t handle, const uint8_t* data, uint16_t len);

// #RADEX-4 H14 (2026-08-25): явный supervision timeout ПЕРЕД подключением.
// Установлено фактом (DEBUG-трасса, radex4_h13_debug_sse.txt): для V1 (наш дефолт)
// ESP-IDF использует свой дефолтный supervision timeout, эмпирически ~6-7с (две
// сессии подряд держались 7.32с и 6.25с - не случайный разброс). Функция должна
// вызываться ДО esp_ble_gattc_open(), не после - поэтому вызывается при определении
// MAC прибора, не в OPEN_EVT хуке.
inline void set_long_supervision_timeout(uint64_t mac_u64) {
  esp_bd_addr_t addr;
  for (int i = 0; i < 6; i++) {
    addr[5 - i] = (uint8_t) ((mac_u64 >> (8 * i)) & 0xFF);
  }
  // min/max interval как MEDIUM (7-9 * 1.25ms), latency=0,
  // supervision_tout=3000 * 10ms = 30s (было ~6-7s дефолт).
  esp_err_t e = esp_ble_gap_set_prefer_conn_params(addr, 0x07, 0x09, 0, 3000);
  if (e != ESP_OK) {
    ESP_LOGW("radex_hook", "set_prefer_conn_params failed, ret=%d", e);
  } else {
    ESP_LOGI("radex_hook", "set_prefer_conn_params OK: supervision_timeout=30s");
  }
}

// #RADEX-4 H9 (2026-08-25): chained READ по ESP_GATTC_OPEN_EVT, в обход on_connect.
//
// Установлено фактом (тик 01:38 брифа radex-ble-loop-run-2026-08-24.md): on_connect
// НИ РАЗУ не сработал за всю петлю #RADEX-4, хотя Connection open случался
// многократно и держался 5-16с (физически достаточно для одного GATT read). Между
// открытием радио-линка и on_connect/ESTABLISHED стоит полный service discovery
// (~45 handle, mr107ion.md) - он не успевает завершиться за доступное окно, и весь
// READ-код (который висел на on_connect/interval+connected()) поэтому не запускался.
//
// Обход: BLEClient::gattc_event_handler (ble_client.cpp:46-59) рассылает КАЖДОЕ
// событие всем зарегистрированным BLEClientNode СРАЗУ после того, как
// BLEClientBase::gattc_event_handler его обработал и вернул true - это происходит
// для ESP_GATTC_OPEN_EVT тоже, задолго до ESTABLISHED. Значит можно перехватить
// OPEN_EVT прямо здесь и запустить READ по уже известным (reverse-engineered)
// handle немедленно, не дожидаясь service discovery вообще - esp_ble_gattc_read_char()
// это низкоуровневый прямой ATT Read Request по handle, протокол Bluetooth не
// требует предварительного discovery, если handle уже известен клиенту.
//
// Цепочка (chained, не по таймеру): получили ответ на текущий handle (успех ИЛИ
// ошибка) -> сразу шлём READ следующего. Так выжимаем максимум READ'ов за то
// время, что радио-линк ещё жив, вместо ожидания фиксированного интервала 4.3с.
class RadexReadHook : public ble_client::BLEClientNode {
 public:
  void set_callback(radex_value_cb_t cb) { this->cb_ = cb; }
  uint32_t get_read_count() const { return this->read_count_; }
  uint32_t get_err_count() const { return this->err_count_; }
  uint32_t get_open_count() const { return this->open_count_; }

  void gattc_event_handler(esp_gattc_cb_event_t event,
                           esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t* param) override {
    if (!this->auto_disc_off_) {
      // H44: один раз, на первом же событии — к этому моменту приложение
      // уже точно зарегистрировано (мы получаем события), значит поздний
      // сброс auto_disc внутри bta_gattc_enable() уже позади.
      BTA_GATTC_AutoDiscoverEnable(0);
      this->auto_disc_off_ = true;
      ESP_LOGW("radex_hook", "*** BTA_GATTC_AutoDiscoverEnable(0) — авто-discovery ВЫКЛЮЧЕН");
    }
    if (event == ESP_GATTC_OPEN_EVT) {
      if (param->open.status != ESP_GATT_OK) return;
      this->open_count_++;
      ESP_LOGI("radex_hook", "OPEN_EVT - starting immediate chained READ (conn_id=%u)",
               param->open.conn_id);
      this->gattc_if_ = gattc_if;
      this->conn_id_ = param->open.conn_id;
      this->cursor_ = 0;
      // H55 (2026-08-28): счётчик значений ЗА ЦИКЛ. read_count_ накопительный за всё
      // время работы платы — в строке «цикл завершён (N значений)» он читался как
      // «получено N за цикл» и врал: 3 → 6 → 10 при четырёх характеристиках.
      this->cycle_reads_ = 0;
      this->issue_read_(this->cursor_);
      return;
    }
    if (event != ESP_GATTC_READ_CHAR_EVT) return;
    if (param->read.status != ESP_GATT_OK) {
      this->err_count_++;
      ESP_LOGW("radex_hook", "READ h=0x%04X status=%d (err#%u)",
               param->read.handle, param->read.status, this->err_count_);
    } else {
      this->read_count_++;
      this->cycle_reads_++;
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
    // Chained: следующий handle сразу, не дожидаясь таймера - ошибка на одном
    // handle не должна останавливать всю цепочку.
    this->cursor_++;
    if (this->cursor_ < HANDLE_COUNT_) {
      this->issue_read_(this->cursor_);
    } else {
      // #RADEX-6 H50 (2026-08-28): цикл опроса завершён — РАЗРЫВАЕМ связь сами.
      // Радону не нужна частая выборка: прибор меряет раз в 60 с, а значение
      // меняется часами (измерено: 113 чтений подряд дали 6 разных значений).
      // Оператор: «нет смысла для радона получать данные чаще раза в 10 минут».
      // Поэтому вместо борьбы за долгую сессию — короткая: подключились,
      // забрали 4 значения, ушли. Прибор не тратит батарею на удержание линка,
      // эфир свободен для Wi-Fi, а короткую сессию ESPHome держит уверенно.
      // #RADEX-6 H51 (2026-08-28): разрыв ОТЛОЖЕННЫЙ, не из колбэка.
      // H50 рвал связь прямо здесь, внутри обработчика GATTC-события. Первый
      // цикл проходил, а следующие четыре подряд падали с
      // «ESP_GATTC_OPEN_EVT in DISCONNECTING state (status=133)»: стек оставался
      // в несогласованном состоянии после разрыва изнутри его же колбэка.
      // Теперь ставим флаг, а рвём в loop() — вне контекста события.
      ESP_LOGI("radex_hook", "цикл опроса завершён: %u из %u значений (всего за сеанс %u) — планирую отключение",
               (unsigned) this->cycle_reads_, (unsigned) HANDLE_COUNT_,
               (unsigned) this->read_count_);
      this->want_disconnect_ = true;
    }
  }

  // Вызывается компонентом вне контекста GATTC-события — здесь рвать безопасно.
  void loop() override {
    if (!this->want_disconnect_) return;
    this->want_disconnect_ = false;
    if (this->parent() != nullptr) {
      ESP_LOGI("radex_hook", "отключаюсь до следующего планового опроса");
      this->parent()->disconnect();
    }
  }

 private:
  static constexpr uint8_t HANDLE_COUNT_ = 4;

  void issue_read_(uint8_t idx) {
    // Радон (0x0049) первым - максимальный шанс получить хотя бы его в короткой сессии.
    static const uint16_t handles[HANDLE_COUNT_] = {0x0049, 0x0058, 0x005E, 0x0040};
    esp_err_t e = esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, handles[idx],
                                          ESP_GATT_AUTH_REQ_NONE);
    if (e != ESP_OK) {
      ESP_LOGW("radex_hook", "issue READ h=0x%04X ret=%d", handles[idx], e);
    }
  }

  radex_value_cb_t cb_ = nullptr;
  uint32_t read_count_ = 0;
  uint32_t cycle_reads_ = 0;  // H55: успешных чтений в ТЕКУЩЕМ цикле
  uint32_t err_count_ = 0;
  uint32_t open_count_ = 0;
  esp_gatt_if_t gattc_if_ = 0;
  uint16_t conn_id_ = 0;
  uint8_t cursor_ = 0;
  bool auto_disc_off_ = false;  // H44: BTA_GATTC_AutoDiscoverEnable(0) вызван
  bool want_disconnect_ = false;  // H51: разрыв запрошен, ждём loop()
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
