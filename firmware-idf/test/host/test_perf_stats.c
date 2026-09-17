/* #RADEX-294: хост-тест накопителя count/sum/max счётчиков производительности.
   Длительности — порядка реальных: страница 830 мс, /api/data 12 мс, 304 — 0 мс. */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "perf_stats.h"

static int fail_tests = 0, total_tests = 0;
#define TEST(name, cond) do { total_tests++; bool ok_ = (cond); if (!ok_) fail_tests++; \
    printf("%s %s\n", ok_ ? "GREEN" : "RED  ", name); } while (0)

int main(void)
{
    perf_acc_t a = {0};
    perf_acc_add(&a, 12);
    perf_acc_add(&a, 830);
    perf_acc_add(&a, 0);
    TEST("test_count_accumulates", a.count == 3);
    TEST("test_sum_accumulates", a.sum_ms == 842);
    TEST("test_max_tracks_largest", a.max_ms == 830);

    perf_acc_t z = {0};
    perf_acc_add(&z, 0);
    TEST("test_zero_duration_counted", z.count == 1 && z.sum_ms == 0);

    TEST("test_ms_between_normal", perf_ms_between(1000000, 3500999) == 2500);
    TEST("test_ms_between_reversed", perf_ms_between(3500000, 1000000) == 0);

    perf_acc_t lit = { .count = 5, .max_ms = 120, .sum_ms = 300 };
    char buf[64];
    int n = perf_acc_json(buf, sizeof(buf), &lit);
    TEST("test_json_format", n > 0 && strcmp(buf, "{\"n\":5,\"sum_ms\":300,\"max_ms\":120}") == 0);
    TEST("test_json_small_buffer", perf_acc_json(buf, 10, &lit) == -1);
    printf("итого (perf_stats): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
