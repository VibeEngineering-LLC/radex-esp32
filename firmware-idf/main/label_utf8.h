// #RADEX-274, оператор 16.09 «да»: кириллица в имени замера. Имя хранится в /data/labels.csv
// строкой `<start>,<имя>` и отдаётся в JSON. Белый список: латиница, цифры, дефис, подчёркивание,
// пробел, кириллица UTF-8 (U+0400..U+04FF); запятая CSV, кавычки, `\` и управляющие — не попадают.
// Лимит — в байтах, символ не разрывается. Хост-тест: test/host/test_label_utf8.c.
#pragma once
#include <stddef.h>

#define RADEX_LABEL_MAX_BYTES 48   /* 24 кириллических символа */

/**
 * Очищает имя замера от недопустимых символов, оставляя только латиницу, цифры,
 * дефис, подчёркивание, пробел и кириллицу (UTF-8).
 */
size_t radex_label_sanitize(const char *in, char *out, size_t out_sz);

/**
 * Экранирует строку для JSON: заменяет специальные символы на их JSON-представления.
 */
int    radex_json_escape(const char *in, char *out, size_t out_sz);

/**
 * Декодирует URL-строку, заменяя %XX на соответствующие байты и + на пробел.
 */
size_t radex_url_decode(const char *in, char *out, size_t out_sz);
