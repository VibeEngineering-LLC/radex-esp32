// #RADEX-283: чистые функции смены прибора (хост-тест test/host/test_target_label.c).
// Идущий замер прежнего прибора сохраняется как pribor-<последние 4 hex MAC>-do-<ГГГГММДД>;
// имя укладывается в предел метки замера (24 символа, латиница/цифры/дефис).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define RADEX_TL_LABEL_MAX 24   /* = RADON_LABEL_MAX в radon_stats.c */

/// Нормализует MAC-адрес из строки вида "xx:xx:xx:xx:xx:xx" или "xx-xx-xx-xx-xx-xx"
/// в строку из 12 шестнадцатеричных символов (без разделителей).
bool radex_mac_normalize(const char *in, char out[13]);

/// Сравнивает два MAC-адреса, нормализуя их перед сравнением.
bool radex_mac_same(const char *a, const char *b);

/// Генерирует метку для нового замера при смене прибора.
bool radex_target_switch_label(const char *old_mac, int year, int month, int day, char *out, size_t out_sz);
