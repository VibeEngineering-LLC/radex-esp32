// #RADEX-294: накопитель времени count/sum/max для счётчиков производительности
// (/api/system -> "perf"). Чистая структура без ESP-IDF и без блокировок —
// синхронизацию обеспечивает вызывающий. Хост-тест test/host/test_perf_stats.c.
#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t count;
    uint32_t max_ms;
    uint64_t sum_ms;
} perf_acc_t;

static inline void perf_acc_add(perf_acc_t *a, uint32_t ms)
{
    a->count++;
    a->sum_ms += ms;
    if (ms > a->max_ms) a->max_ms = ms;
}

// Миллисекунды между двумя отметками esp_timer_get_time() (микросекунды).
// Обратный порядок отметок даёт 0, больше UINT32_MAX мс — насыщение.
static inline uint32_t perf_ms_between(int64_t t0_us, int64_t t1_us)
{
    int64_t d = (t1_us - t0_us) / 1000;
    if (d < 0) return 0;
    if (d > (int64_t)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)d;
}

// {"n":..,"sum_ms":..,"max_ms":..}; длина записанного или -1, если буфер мал.
static inline int perf_acc_json(char *buf, size_t size, const perf_acc_t *a)
{
    int n = snprintf(buf, size, "{\"n\":%" PRIu32 ",\"sum_ms\":%" PRIu64 ",\"max_ms\":%" PRIu32 "}",
                     a->count, a->sum_ms, a->max_ms);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}
