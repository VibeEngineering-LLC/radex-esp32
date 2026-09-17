// #RADEX-293: калибровка времени журнала на лету + дедуп с общей историей.
// Эпоха time_raw не установлена (reports/radex-ble-sniff-app-2026-09-16.md,
// расхождение ~1202 с с характеристикой 0x0039 не разрешено). Точка отсчёта —
// часы ШЛЮЗА (NTP, is_valid_time в radon_stats.c), а не характеристика 0x0039
// прибора: та не читается нашей прошивкой (grep 0x0039 по main/*.c — 0 совпадений),
// её чтение добавило бы новую GATT-операцию в сеанс журнала, а запись в неё
// сдвигает часы прибора (R/W, риск порчи прибора) — часы шлюза безопаснее и
// уже используются во всём модуле. Чистый заголовок, хост-тест
// test/host/test_journal_calib.c.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// #RADEX-293 аудит находка 2: границы валидности часов шлюза — ОБЩИЕ для is_valid_time()
// (radon_stats.c) и калибровки журнала: нижняя одной хватало обычному опросу (одна плохая
// точка), но калибровка молча сдвигает до 64 точек разом одним аномальным board_unix_now —
// нужна и верхняя. +20 лет от нижней границы — запас без пересборки прошивки годами, но
// отсекающий скачок NTP/RTC далеко вперёд. Хост-тест: test/host/test_journal_calib.c.
#define RADON_TIME_MIN 1700000000LL          // после 2023 года
#define RADON_TIME_MAX (RADON_TIME_MIN + 20LL * 365 * 24 * 3600)   // ~2043 год
static inline int radex_time_valid(time_t ts)
{
    return (int64_t)ts >= RADON_TIME_MIN && (int64_t)ts <= RADON_TIME_MAX;
}

// offset = часы шлюза в момент завершения сеанса − время последней записи
// журнала (сводка, summary.time_raw). Разность time_raw соседних записей
// (кратна 600 с) офсет не меняет — сдвигается только точка отсчёта.
static inline int64_t radex_journal_calib_offset(time_t board_unix_now, uint32_t summary_time_raw)
{
    return (int64_t)board_unix_now - (int64_t)summary_time_raw;
}

// Абсолютное время записи = record.time_raw + offset.
static inline time_t radex_journal_calib_abs(uint32_t record_time_raw, int64_t offset)
{
    return (time_t)((int64_t)record_time_raw + offset);
}

// true — в пределах window_sec от candidate уже есть точка в existing[n]:
// запись журнала дублирует то, что обычный опрос уже записал, пропустить.
// Пустой existing (n==0) — ВСЕГДА "не покрыто": история пуста в этом окне —
// это и есть пробел, который журнал обязан заполнить, не промолчать о нём.
static inline int radex_journal_dedup_covered(const time_t *existing, size_t n,
                                              time_t candidate, int window_sec)
{
    for (size_t i = 0; i < n; i++) {
        int64_t d = (int64_t)existing[i] - (int64_t)candidate;
        if (d < 0) d = -d;
        if (d <= (int64_t)window_sec) return 1;
    }
    return 0;
}
