// Последние показания прибора + время их получения. Пишется из BLE-колбэка,
// читается HTTP-обработчиками — отсюда мьютекс.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float    radon_last;    // Бк/м³
    float    radon_avg;     // Бк/м³, среднее самого прибора
    float    temperature;   // °C
    float    humidity;      // %
    /* #RADEX-18: значения, которые прибор считает сам по ВСЕМ своим замерам.
       Наше среднее строится по выборке раз в 10 минут и потому грубее. */
    float    sko_avg;       // СКО среднего прибора, Бк/м3 (характеристика 0x0043)
    uint32_t t_izm_sec;     // время измерения прибора, с (0x0046)
    int64_t  ts_last_us;    // esp_timer_get_time() последнего любого значения
    uint32_t samples;       // сколько значений принято всего
    bool     valid;         // было ли хоть одно значение
} radex_data_t;

void radex_data_init(void);
void radex_data_put(uint16_t handle, float value);
void radex_data_get(radex_data_t *out);
int  radex_data_json(char *buf, size_t len);
