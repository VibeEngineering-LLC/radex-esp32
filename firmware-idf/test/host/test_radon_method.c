/* #RADEX-271: хост-тест порога правила §7.1.2 (10 месяцев = 300 сут) на границе. */
#include <stdio.h>
#include "radon_method.h"

static int fail_tests = 0, total_tests = 0;
#define TEST(name, cond) do { total_tests++; bool ok_ = (cond); if (!ok_) fail_tests++; \
    printf("%s %s\n", ok_ ? "GREEN" : "RED  ", name); } while (0)

int main(void)
{
    /* 9,9 мес = 297 сут — правило ещё не применяется */
    TEST("test_rule_9_9_months_not_applied", !radon_rule_months_reached(297));
    /* 10,0 мес = 300 сут — применяется */
    TEST("test_rule_10_months_applied", radon_rule_months_reached(300) && radon_rule_months_reached(341));
    TEST("test_rule_short_test", !radon_rule_months_reached(0) && !radon_rule_months_reached(4));
    printf("итого (method): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
