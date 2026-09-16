// #RADEX-271, Цапалов 15.09 (документ 2, п.5), оператор 16.09 «как Цапалов говорит»:
// правило §7.1.2 «критерий (1) не выполнен и продолжительность теста достигла порога»
// применяется после 10 месяцев вместо 9 — «целесообразно (более надёжно)».
// Месяц методики — 30 суток (год таблиц UV/Kp = 360 суток, radon_stats.c).
// Чистый заголовок: хост-тест test/host/test_radon_method.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RADON_RULE_MONTHS        10
#define RADON_DAYS_PER_MONTH     30
#define RADON_RULE_DAYS          (RADON_RULE_MONTHS * RADON_DAYS_PER_MONTH)   /* 300 сут */

/* Продолжительность теста days (полных суток) достигла порога правила §7.1.2. */
static inline bool radon_rule_months_reached(uint32_t days)
{
    return days >= (uint32_t)RADON_RULE_DAYS;
}

/* Аудит 267 D3: решение по критериям (1), (2) и правилу §7.1.2 — чистая функция;
   вердикт платы (radon_stats.c) берётся из неё, поэтому хост-тест покрывает саму строку решения. */
#define RADON_DECIDE_COMPLIES   1
#define RADON_DECIDE_EXCEEDS    2
#define RADON_DECIDE_UNCERTAIN  3
static inline int radon_method_decide(bool crit1_met, bool crit2_met, int days)
{
    if (crit1_met) return RADON_DECIDE_COMPLIES;
    if (crit2_met || (days > 0 && radon_rule_months_reached((uint32_t)days))) return RADON_DECIDE_EXCEEDS;
    return RADON_DECIDE_UNCERTAIN;
}
