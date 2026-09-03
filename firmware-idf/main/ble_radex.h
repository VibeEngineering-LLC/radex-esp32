// BLE-клиент к Radex MR107ion. Реализация — ble_radex.c.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Известные handle прибора (GATT-карта FE651700, RE 2026-06-13).
#define RADEX_H_RADON_LAST 0x0049  // float32 LE, Бк/м³
#define RADEX_H_RADON_AVG  0x0040  // float32 LE, Бк/м³
#define RADEX_H_TEMP       0x0058  // int16 LE / 10, °C
#define RADEX_H_HUMIDITY   0x005E  // uint8, %
// #RADEX-18: прибор считает их сам по всем своим замерам — точнее нашей выборки.
#define RADEX_H_SKO_AVG    0x0043  // float32 LE, СКО среднего, Бк/м³
#define RADEX_H_T_IZM      0x0046  // uint32 LE, время измерения прибора, с

// Вызывается из BLE-задачи на каждое успешно прочитанное значение.
// Обработчик обязан быть коротким и не блокировать: он исполняется
// в контексте GATTC-события, за ним сразу идёт следующее чтение.
typedef void (*ble_radex_cb_t)(uint16_t handle, float value);

// Поднимает стек и запускает подключение с бесконечными переподключениями.
void ble_radex_start(ble_radex_cb_t cb);

// ── Выбор прибора ────────────────────────────────────────────────────────
// Адрес прибора в прошивку не зашит: берётся из NVS, а если там пусто —
// ищется в эфире и предъявляется пользователю в Web UI.
bool ble_radex_has_target(void);
bool ble_radex_scanning(void);
int  ble_radex_found_json(char *buf, size_t len);
bool ble_radex_set_target(const char *mac_str);   // "AA:BB:CC:DD:EE:FF"
// #RADEX-188: адрес выбранного прибора строкой для Web UI. Пишет в buf
// "AA:BB:CC:DD:EE:FF" и возвращает true; если прибор не выбран — false.
bool ble_radex_target_mac(char *buf, size_t len);
// #RADEX-190: забыть выбранный прибор и снова начать поиск в эфире. Нужно,
// когда плата сама привязалась к чужому Radex по соседству: скан идёт ТОЛЬКО
// пока прибор не выбран, поэтому без сброса свой прибор в списке не появится.
void ble_radex_clear_target(void);

// Счётчики для диагностики.
bool     ble_radex_connected(void);
uint32_t ble_radex_reads_ok(void);
uint32_t ble_radex_read_errors(void);
uint32_t ble_radex_disconnects(void);
/* #RADEX-84: неудачные попытки ОТКРЫТЬ соединение — считаются отдельно от
   разрывов уже установленного: смешение давало счётчик в десятки тысяч. */
uint32_t ble_radex_open_fails(void);
