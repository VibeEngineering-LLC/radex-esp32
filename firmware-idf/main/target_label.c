#include "target_label.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Преобразовать MAC-адрес в верхний регистр, проверить формат и записать 12 символов без разделителей
bool radex_mac_normalize(const char *in, char out[13]) {
    if (in == NULL || out == NULL) return false;

    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\r' || in[n - 1] == '\n')) {
        n--;
    }

    if (n != 17) return false;

    char sep = in[2];
    if (sep != ':' && sep != '-') return false;

    for (size_t i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (in[i] != sep) return false;
        } else {
            if (!isxdigit((unsigned char)in[i])) return false;
        }
    }

    size_t j = 0;
    for (size_t i = 0; i < 17; i++) {
        if (i % 3 != 2) {
            out[j++] = toupper((unsigned char)in[i]);
        }
    }
    out[12] = '\0';
    return true;
}

// Сравнить два MAC-адреса
bool radex_mac_same(const char *a, const char *b) {
    char na[13], nb[13];
    return radex_mac_normalize(a, na) && radex_mac_normalize(b, nb) && memcmp(na, nb, 12) == 0;
}

// Сформировать метку для нового замера при смене прибора
bool radex_target_switch_label(const char *old_mac, int year, int month, int day, char *out, size_t out_sz) {
    if (out == NULL || out_sz < RADEX_TL_LABEL_MAX + 1) return false;

    out[0] = '\0';

    char n[13];
    if (!radex_mac_normalize(old_mac, n)) return false;

    if (year < 2000 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31) return false;

    int len = snprintf(out, out_sz, "pribor-%s-do-%04d%02d%02d", n + 8, year, month, day);
    if (len < 0 || len > RADEX_TL_LABEL_MAX) {
        out[0] = '\0';
        return false;
    }

    return true;
}
