// #RADEX-294: разбор заголовков запроса страницы "/" — Accept-Encoding и
// If-None-Match. Чистые функции без ESP-IDF, хост-тест test/host/test_http_cache.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool http_is_ows(char c) { return c == ' ' || c == '\t'; }

// Сравнение токена длины n с литералом в нижнем регистре, без учёта регистра.
static inline bool http_token_ieq(const char *s, size_t n, const char *lit)
{
    if (n != strlen(lit)) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != lit[i]) return false;
    }
    return true;
}

// Хвост элемента после имени кодировки содержит q=0 (0, 0.0, 0.00, 0.000):
// клиент явно запретил эту кодировку.
static inline bool http_q_zero(const char *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        if ((p[i] == 'q' || p[i] == 'Q') && p[i + 1] == '=') {
            size_t j = i + 2;
            if (j >= n || p[j] != '0') return false;
            for (j++; j < n && !http_is_ows(p[j]); j++)
                if (p[j] != '.' && p[j] != '0') return false;
            return true;
        }
    }
    return false;
}

// Клиент принимает gzip: в списке есть gzip, x-gzip или *, и не с q=0.
static inline bool accept_gzip(const char *hdr)
{
    if (hdr == NULL) return false;
    const char *p = hdr;
    while (*p) {
        while (http_is_ows(*p) || *p == ',') p++;
        const char *item = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - item);
        size_t name = 0;
        while (name < len && item[name] != ';' && !http_is_ows(item[name])) name++;
        if ((http_token_ieq(item, name, "gzip") || http_token_ieq(item, name, "x-gzip")
             || http_token_ieq(item, name, "*"))
            && !http_q_zero(item + name, len - name))
            return true;
    }
    return false;
}

// Слабое сравнение для If-None-Match (RFC 9110 §13.1.2): префикс W/ не
// учитывается, кавычки — часть значения. etag — наш тег вместе с кавычками.
static inline bool etag_match(const char *inm, const char *etag)
{
    if (inm == NULL || etag == NULL) return false;
    size_t elen = strlen(etag);
    const char *p = inm;
    while (*p) {
        while (http_is_ows(*p) || *p == ',') p++;
        if (*p == '*') return true;
        if (p[0] == 'W' && p[1] == '/') p += 2;
        if (*p != '"') {
            while (*p && *p != ',') p++;
            continue;
        }
        const char *q = strchr(p + 1, '"');
        if (q == NULL) return false;
        size_t len = (size_t)(q - p) + 1;
        if (len == elen && memcmp(p, etag, elen) == 0) return true;
        p = q + 1;
        while (*p && *p != ',') p++;
    }
    return false;
}
