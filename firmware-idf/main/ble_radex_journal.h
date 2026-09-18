// #RADEX-281: чтение журнала Radex по NUS, сериализовано с круговым опросом ble_radex.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
/* #USB-1: при USB-транспорте (radex_usb.c) GATT-часть не нужна — её типы есть только
   со включённым Bluedroid, поэтому объявления ниже для USB-сборки скрыты. */
#if !CONFIG_RADEX_TRANSPORT_USB
#include <esp_gattc_api.h>
#endif
#include "radex_journal_parse.h"   /* #RADEX-293: radex_journal_t для ble_radex_journal_get_result */

void ble_radex_journal_init(void);
void ble_radex_journal_request(void);          // из Web: поставить в очередь
bool ble_radex_journal_busy(void);             // сеанс идёт — опрос не начинать
bool ble_radex_journal_pending(void);
#if !CONFIG_RADEX_TRANSPORT_USB
// Из главного цикла ble_radex (100 мс). can_start: связь есть и круг опроса не идёт.
// conn_gen растёт на каждом успешном open: сеанс снимается, если соединение сменилось.
void ble_radex_journal_tick(bool can_start, bool connected, uint32_t conn_gen,
                            esp_gatt_if_t gattc_if, uint16_t conn_id, const uint8_t *bda);
void ble_radex_journal_on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *param);
void ble_radex_journal_on_disconnect(void);
void ble_radex_journal_set_mtu(uint16_t mtu);          // из GATTS_MTU_EVT ble_radex.c
#endif
int  ble_radex_journal_json(char *buf, size_t len);   // JSON для GET /api/journal
// #RADEX-293: копия последнего результата сеанса (под тем же мьютексом, что и
// ble_radex_journal_json) — для radon_stats_journal_preview/save. false — out==NULL.
bool ble_radex_journal_get_result(radex_journal_t *out);
