/* #RADEX-274: хост-тест санитайзера имени замера (кириллица, лимит в байтах, CSV/JSON). Сборка: test/host/run.ps1 */
#include <stdio.h>
#include <string.h>
#include "label_utf8.h"

static int g_fail_tests = 0, g_cur_fail = 0, g_total_tests = 0;
#define CHECK(cond) do { if (!(cond)) { g_cur_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define RUN(fn) do { g_cur_fail = 0; g_total_tests++; fn(); printf("%s %s\n", g_cur_fail ? "RED  " : "GREEN", #fn); if (g_cur_fail) g_fail_tests++; } while (0)

void test_cyrillic_with_space() {
    char o[64];
    size_t n = radex_label_sanitize("Спальня 1", o, sizeof o);
    CHECK(n == strlen("Спальня 1"));
    CHECK(strcmp(o, "Спальня 1") == 0);
}

void test_cyrillic_dash_underscore() {
    char o[64];
    size_t n = radex_label_sanitize("Кухня-2_тест", o, sizeof o);
    CHECK(n == strlen("Кухня-2_тест"));
    CHECK(strcmp(o, "Кухня-2_тест") == 0);
}

void test_latin_unchanged() {
    char o[64];
    size_t n = radex_label_sanitize("kvartira-1", o, sizeof o);
    CHECK(n == strlen("kvartira-1"));
    CHECK(strcmp(o, "kvartira-1") == 0);
}

void test_utf8_boundary() {
    char in[128] = "a";
    for (int i = 0; i < 24; ++i) strcat(in, "ж");
    char o[64];
    size_t n = radex_label_sanitize(in, o, sizeof o);
    CHECK(n == 47);
    CHECK(strlen(o) == 47);
    CHECK(((unsigned char)o[46] >= 0x80) && ((unsigned char)o[46] <= 0xBF));
    CHECK((unsigned char)o[45] == 0xD0);
}

void test_quote_and_csv_separator() {
    char o[64];
    size_t n = radex_label_sanitize("a\"b,c|d;e\\f", o, sizeof o);
    CHECK(n == strlen("abcdef"));
    CHECK(strcmp(o, "abcdef") == 0);
}

void test_control_and_spaces() {
    char o[64];
    size_t n = radex_label_sanitize("  x\t y  z  ", o, sizeof o);
    CHECK(n == strlen("x y z"));
    CHECK(strcmp(o, "x y z") == 0);
}

void test_other_scripts_dropped() {
    char o[64];
    size_t n = radex_label_sanitize("é日本x", o, sizeof o);
    CHECK(n == strlen("x"));
    CHECK(strcmp(o, "x") == 0);
}

void test_json_escape() {
    char e[64];
    CHECK(radex_json_escape("a\"b\\c\x01", e, sizeof e) > 0);
    CHECK(strcmp(e, "a\\\"b\\\\c\\u0001") == 0);
    CHECK(radex_json_escape("Спальня 1", e, sizeof e) > 0);
    CHECK(strcmp(e, "Спальня 1") == 0);
    char small[4];
    CHECK(radex_json_escape("abcdef", small, sizeof small) == -1);
}

void test_url_decode() {
    char d[64];
    radex_url_decode("%D0%A1%D0%BF+1", d, sizeof d);
    CHECK(strcmp(d, "Сп 1") == 0);
    radex_url_decode("100%zz", d, sizeof d);
    CHECK(strcmp(d, "100%zz") == 0);
}

int main() {
    RUN(test_cyrillic_with_space);
    RUN(test_cyrillic_dash_underscore);
    RUN(test_latin_unchanged);
    RUN(test_utf8_boundary);
    RUN(test_quote_and_csv_separator);
    RUN(test_control_and_spaces);
    RUN(test_other_scripts_dropped);
    RUN(test_json_escape);
    RUN(test_url_decode);
    printf("итого (label): красных тестов %d из %d\n", g_fail_tests, g_total_tests);
    return g_fail_tests ? 1 : 0;
}
