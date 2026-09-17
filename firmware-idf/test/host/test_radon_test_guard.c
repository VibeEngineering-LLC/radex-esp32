/* #RADEX-291: хост-тест защиты «Начало теста» — активный явный тест не должен молча перезаписываться. */
#include <stdio.h>
#include <stdbool.h>
#include "radon_test_guard.h"

static int fail_tests = 0, total_tests = 0;
#define TEST(name, cond) do { total_tests++; bool ok_ = (cond); if (!ok_) fail_tests++; \
    printf("%s %s\n", ok_ ? "GREEN" : "RED  ", name); } while (0)

int main(void)
{
    TEST("test_active_explicit_blocks_start", radon_test_start_blocked(1000, 0) == true);
    TEST("test_finished_test_allows_start", radon_test_start_blocked(1000, 2000) == false);
    TEST("test_never_started_allows_start", radon_test_start_blocked(0, 0) == false);
    printf("итого (guard): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
