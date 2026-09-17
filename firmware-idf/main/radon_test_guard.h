// #RADEX-291, оператор: «кнопка "Начало теста" непонятно что тест уже идёт,
// а это начало нового». Причина: radon_stats_start_test_locked() молча
// перезаписывала tstart, осиротив файл прежнего замера (класс #RADEX-283,
// но без явного действия оператора «сменил прибор»). Чистая функция —
// хост-тест test/host/test_radon_test_guard.c.
#pragma once
#include <stdbool.h>
#include <time.h>

// true — явный тест начат (tstart>0) и ещё не завершён (tend<=0): второй
// "Начало теста" отклоняется, а не перезаписывает начало молча.
static inline bool radon_test_start_blocked(time_t tstart, time_t tend)
{
    return tstart > 0 && tend <= 0;
}
