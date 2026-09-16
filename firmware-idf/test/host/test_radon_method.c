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
    /* аудит 267 D3: строка решения вердикта — крит.(1) и (2) не выполнены, решает только правило */
    TEST("test_decide_rule_boundary", radon_method_decide(false, false, 297) == RADON_DECIDE_UNCERTAIN
                                   && radon_method_decide(false, false, 300) == RADON_DECIDE_EXCEEDS);
    TEST("test_decide_criteria", radon_method_decide(true, true, 400) == RADON_DECIDE_COMPLIES
                              && radon_method_decide(false, true, 5) == RADON_DECIDE_EXCEEDS);
    printf("итого (method): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
