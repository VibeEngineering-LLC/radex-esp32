#include "radex_data.h"
#include "ble_radex.h"

#include <stdio.h>
#include <string.h>
#include <esp_timer.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static radex_data_t      s_data;
static SemaphoreHandle_t s_lock;

void radex_data_init(void)
{
    memset(&s_data, 0, sizeof(s_data));
    /* #RADEX-18: неизмеренная величина — НЕ ноль. С ротацией характеристик
       (прибор отдаёт лишь 4 за сессию) температура и влажность приходят не в
       каждом круге, и ноль на экране читался бы как «0,0 °C» — измерение,
       которого не было. NAN отдаётся наружу как null, страница рисует прочерк. */
    s_data.radon_last  = NAN;
    s_data.radon_avg   = NAN;
    s_data.temperature = NAN;
    s_data.humidity    = NAN;
    s_data.sko_avg     = NAN;
    s_lock = xSemaphoreCreateMutex();
}

void radex_data_put(uint16_t handle, float value)
{
    if (!s_lock) return;
    // Колбэк зовётся из GATTC-события: держим блокировку минимально,
    // никакого ввода-вывода под ней.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    switch (handle) {
        case RADEX_H_RADON_LAST: s_data.radon_last  = value; break;
        case RADEX_H_RADON_AVG:  s_data.radon_avg   = value; break;
        case RADEX_H_TEMP:       s_data.temperature = value; break;
        case RADEX_H_HUMIDITY:   s_data.humidity    = value; break;
        case RADEX_H_SKO_AVG:    s_data.sko_avg     = value; break;
        case RADEX_H_T_IZM:      s_data.t_izm_sec   = (uint32_t)value; break;
        default: xSemaphoreGive(s_lock); return;   // чужой handle не считаем
    }
    s_data.ts_last_us = esp_timer_get_time();
    s_data.samples++;
    s_data.valid = true;
    xSemaphoreGive(s_lock);
}

void radex_data_get(radex_data_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_data;
    xSemaphoreGive(s_lock);
}

int radex_data_json(char *buf, size_t len)
{
    radex_data_t d;
    radex_data_get(&d);
    // Возраст последнего значения важнее самого значения: он показывает,
    // живая связь или числа застыли (ровно та ошибка чтения показаний,
    // на которую мы попались 2026-08-27 — экран показывал старые данные).
    int64_t age_ms = d.valid ? (esp_timer_get_time() - d.ts_last_us) / 1000 : -1;
    /* Число либо null: null означает «ещё не измерено», ноль — измеренный ноль. */
    char s_last[16], s_avg[16], s_temp[16], s_hum[16], s_sko[16];
    #define NUM(dst, val, fmt)         do { if (isnan(val)) snprintf(dst, sizeof(dst), "null");              else snprintf(dst, sizeof(dst), fmt, val); } while (0)
    NUM(s_last, d.radon_last, "%.2f");
    NUM(s_avg,  d.radon_avg,  "%.2f");
    NUM(s_temp, d.temperature, "%.1f");
    NUM(s_hum,  d.humidity,   "%.0f");
    NUM(s_sko,  d.sko_avg,    "%.2f");
    #undef NUM

    return snprintf(buf, len,
        "{\"valid\":%s,\"radon_last\":%s,\"radon_avg\":%s,"
        "\"temperature\":%s,\"humidity\":%s,\"age_ms\":%lld,\"samples\":%lu,"
        "\"ble_connected\":%s,\"reads_ok\":%lu,\"read_errors\":%lu,\"disconnects\":%lu,\"open_fails\":%lu,"
        "\"sko_avg\":%s,\"t_izm_sec\":%lu}",
        d.valid ? "true" : "false", s_last, s_avg,
        s_temp, s_hum, (long long)age_ms, (unsigned long)d.samples,
        ble_radex_connected() ? "true" : "false",
        (unsigned long)ble_radex_reads_ok(),
        (unsigned long)ble_radex_read_errors(),
        (unsigned long)ble_radex_disconnects(),
        (unsigned long)ble_radex_open_fails(),
        s_sko, (unsigned long)d.t_izm_sec);
}
