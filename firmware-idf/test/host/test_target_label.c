/* #RADEX-283: хост-тест имени сохраняемого замера и сравнения MAC при смене прибора. Сборка: test/host/run.ps1 */
#include <stdio.h>
#include <string.h>
#include "target_label.h"

static int g_fail_tests = 0;
static int g_cur_fail;
static int g_total_tests = 0;

#define CHECK(cond) do { if (!(cond)) { g_cur_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define RUN(fn) do { g_cur_fail = 0; g_total_tests++; fn(); printf("%s %s\n", g_cur_fail ? "RED  " : "GREEN", #fn); if (g_cur_fail) g_fail_tests++; } while (0)

static void test_normalize_forms(void) {
    char o[13];
    CHECK(radex_mac_normalize("AA:BB:CC:DD:EE:FF", o) && strcmp(o, "AABBCCDDEEFF") == 0);
    CHECK(radex_mac_normalize("aa:bb:cc:dd:ee:0f", o) && strcmp(o, "AABBCCDDEE0F") == 0);
    CHECK(radex_mac_normalize("11-22-33-44-55-66", o) && strcmp(o, "112233445566") == 0);
    CHECK(radex_mac_normalize("AA:BB:CC:DD:EE:FF\r\n", o));
}

static void test_normalize_rejects(void) {
    char o[13];
    CHECK(!radex_mac_normalize(NULL, o));
    CHECK(!radex_mac_normalize("", o));
    CHECK(!radex_mac_normalize("AA:BB:CC:DD:EE", o));
    CHECK(!radex_mac_normalize("AA:BB:CC:DD:EE:FF:00", o));
    CHECK(!radex_mac_normalize("AA:BB:CC:DD:EE:GG", o));
    CHECK(!radex_mac_normalize("AA:BB-CC:DD:EE:FF", o));
    CHECK(!radex_mac_normalize("AABBCCDDEEFF00000", o));
    CHECK(!radex_mac_normalize(" AA:BB:CC:DD:EE:FF", o));
}

static void test_same_case_and_separator(void) {
    CHECK(radex_mac_same("AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff"));
    CHECK(radex_mac_same("11:22:33:44:55:66\n", "11:22:33:44:55:66"));
}

static void test_same_last_octet_differs(void) {
    CHECK(!radex_mac_same("AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FE"));
    CHECK(!radex_mac_same("AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EF:FF"));
}

static void test_same_invalid(void) {
    CHECK(!radex_mac_same("AA:BB:CC:DD:EE:FF", "bad"));
    CHECK(!radex_mac_same(NULL, "AA:BB:CC:DD:EE:FF"));
}

static void test_label_format(void) {
    char l[32];
    CHECK(radex_target_switch_label("aa:bb:cc:dd:ee:ff", 2026, 9, 16, l, sizeof l) && strcmp(l, "pribor-EEFF-do-20260916") == 0);
    CHECK(strlen(l) <= RADEX_TL_LABEL_MAX);

    CHECK(radex_target_switch_label("11:22:33:44:55:66", 2027, 1, 5, l, sizeof l) && strcmp(l, "pribor-5566-do-20270105") == 0);

    for (size_t i = 0; i < strlen(l); ++i) {
        char c = l[i];
        CHECK((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
    }
}

static void test_label_rejects(void) {
    char l[32];
    char small[10];

    CHECK(!radex_target_switch_label("bad", 2026, 9, 16, l, sizeof l) && l[0] == '\0');
    CHECK(!radex_target_switch_label("AA:BB:CC:DD:EE:FF", 1970, 1, 1, l, sizeof l));
    CHECK(!radex_target_switch_label("AA:BB:CC:DD:EE:FF", 2026, 13, 1, l, sizeof l));
    CHECK(!radex_target_switch_label("AA:BB:CC:DD:EE:FF", 2026, 9, 16, small, sizeof small));
    CHECK(!radex_target_switch_label("AA:BB:CC:DD:EE:FF", 2026, 9, 16, NULL, 32));
}

int main(void) {
    RUN(test_normalize_forms);
    RUN(test_normalize_rejects);
    RUN(test_same_case_and_separator);
    RUN(test_same_last_octet_differs);
    RUN(test_same_invalid);
    RUN(test_label_format);
    RUN(test_label_rejects);

    printf("итого (target): красных тестов %d из %d\n", g_fail_tests, g_total_tests);
    return g_fail_tests ? 1 : 0;
}
