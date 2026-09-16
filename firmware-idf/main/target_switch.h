// #RADEX-283: смена прибора с автоматическим сохранением идущего замера.
// Оператор 16.09 делал это вручную: test finish -> tests/save -> test start.
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Обработать тело POST /api/target (MAC "AA:BB:CC:DD:EE:FF"). Пишет JSON-ответ в out.
   Тот же MAC — ничего не делать (без перезапуска). Первый выбор прибора — только запомнить.
   Возвращает HTTP-код: 200 или 400. *restart = true, если прибор сменён и плату нужно перезапустить. */
int radex_target_switch(const char *mac_body, char *out, size_t out_sz, bool *restart);
