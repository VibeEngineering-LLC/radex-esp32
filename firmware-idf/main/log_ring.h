// Кольцевой буфер лога в памяти + история показаний для графиков.
//
// Смысл: видеть работу платы в браузере, без UART и без внешних инструментов.
// Это ровно та слепота, из-за которой 27.08 полдня ушло на диагностику вслепую.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void log_ring_init(void);                       // перехват esp_log
int  log_ring_dump(char *buf, size_t len);      // весь буфер, старое → новое
void log_ring_clear(void);                      // #RADEX-105: очистить кольцо

// История показаний: кольцо на HIST_N точек, шаг задаётся вызывающим.
#define HIST_N 120
typedef struct {
    float    radon;
    float    temp;
    uint8_t  hum;
    uint32_t t_sec;      // uptime платы на момент точки
} hist_point_t;

void log_ring_hist_push(float radon, float temp, float hum);
int  log_ring_hist_json(char *buf, size_t len);
