// #RADEX-281: чтение журнала Radex по NUS, сериализовано с круговым опросом ble_radex.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <esp_gattc_api.h>

void ble_radex_journal_init(void);
void ble_radex_journal_request(void);          // из Web: поставить в очередь
bool ble_radex_journal_busy(void);             // сеанс идёт — опрос не начинать
bool ble_radex_journal_pending(void);
// Из главного цикла ble_radex (100 мс). can_start: связь есть и круг опроса не идёт.
void ble_radex_journal_tick(bool can_start, esp_gatt_if_t gattc_if, uint16_t conn_id, const uint8_t *bda);
void ble_radex_journal_on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *param);
void ble_radex_journal_on_disconnect(void);
int  ble_radex_journal_json(char *buf, size_t len);   // JSON для GET /api/journal
