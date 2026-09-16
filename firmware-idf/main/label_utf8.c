/* #RADEX-274: санитайзер имени замера с кириллицей, JSON-экранирование, URL-декодирование.
   Чистый модуль (без ESP-IDF), хост-тест test/host/test_label_utf8.c. */
#include "label_utf8.h"
#include <string.h>
#include <stdio.h>

// Очищает имя замера, оставляя только разрешённые символы: латиница, цифры, дефис, подчёркивание, пробел и кириллица
size_t radex_label_sanitize(const char *in, char *out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }

    size_t limit = out_sz - 1;
    if (limit > RADEX_LABEL_MAX_BYTES) {
        limit = RADEX_LABEL_MAX_BYTES;
    }
    size_t o = 0;
    size_t i = 0;

    while (in[i] != '\0') {
        unsigned char c = (unsigned char)in[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            if (o + 1 > limit) break;
            out[o++] = (char)c;
            i++;
            continue;
        }

        if (c == ' ') {
            if (o > 0 && out[o - 1] != ' ') {
                if (o + 1 > limit) break;
                out[o++] = ' ';
            }
            i++;
            continue;
        }

        if (c >= 0xD0 && c <= 0xD3) {
            unsigned char c2 = (unsigned char)in[i + 1];
            if (c2 >= 0x80 && c2 <= 0xBF) {
                if (o + 2 > limit) break;   /* символ целиком или никак */
                out[o++] = (char)c;
                out[o++] = (char)c2;
                i += 2;
                continue;
            }
        }

        // Пропускаем неразрешённые байты
        size_t n;
        if (c >= 0xC2 && c <= 0xDF) {
            n = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4;
        } else {
            n = 1;
        }

        while (n > 0 && in[i] != '\0') {
            i++;
            n--;
        }
        if (n > 0) {
            break; // Не нашли завершение последовательности
        }
    }

    // Убираем завершающие пробелы
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
    return o;
}

// Экранирует строку для JSON: заменяет специальные символы на экранированные последовательности
int radex_json_escape(const char *in, char *out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }

    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        int k = 0;

        switch (c) {
            case '"':
                k = 2;
                if (o + k > out_sz - 1) goto error;
                out[o++] = '\\';
                out[o++] = '"';
                break;
            case '\\':
                k = 2;
                if (o + k > out_sz - 1) goto error;
                out[o++] = '\\';
                out[o++] = '\\';
                break;
            default:
                if (c < 0x20) {
                    char u[7];
                    int len = snprintf(u, sizeof(u), "\\u%04x", c);
                    if (o + len > out_sz - 1) goto error;
                    memcpy(out + o, u, len);
                    o += len;
                } else {
                    k = 1;
                    if (o + k > out_sz - 1) goto error;
                    out[o++] = (char)c;
                }
        }
    }

    out[o] = '\0';
    return (int)o;

error:
    out[o] = '\0';
    return -1;
}

// Декодирует URL-кодированную строку: заменяет %XX на соответствующий байт, + на пробел
static int hexval(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

size_t radex_url_decode(const char *in, char *out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }

    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o < out_sz - 1; ) {
        char c = in[i];
        if (c == '+') {
            out[o++] = ' ';
            i++;
        } else if (c == '%' && in[i + 1] != '\0' && in[i + 2] != '\0') {
            int h1 = hexval(in[i + 1]);
            int h2 = hexval(in[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                out[o++] = (char)(h1 * 16 + h2);
                i += 3;
            } else {
                out[o++] = c;
                i++;
            }
        } else {
            out[o++] = c;
            i++;
        }
    }

    out[o] = '\0';
    return o;
}
