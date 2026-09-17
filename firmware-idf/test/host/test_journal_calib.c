/* #RADEX-293: хост-тест калибровки времени журнала и дедупликации с историей. */
#include <stdio.h>
#include "journal_calib.h"

static int fail_tests = 0, total_tests = 0;
#define TEST(name, cond) do { total_tests++; int ok_ = (cond); if (!ok_) fail_tests++; \
    printf("%s %s\n", ok_ ? "GREEN" : "RED  ", name); } while (0)

int main(void)
{
    /* Реальные числа из отчёта (Захват 3, reports/radex-ble-sniff-app-2026-09-16.md):
       summary.time_raw = time_raw записи №3 = 0x323da9a9; №2 = 0x323da751, разница 600 с.
       board_unix_now — синтетическое NTP-время шлюза в момент завершения сеанса. */
    const uint32_t SUM_TIME_RAW = 0x323da9a9;
    const uint32_t REC2_TIME_RAW = 0x323da751;
    const time_t BOARD_NOW = 1789600000;

    int64_t offset = radex_journal_calib_offset(BOARD_NOW, SUM_TIME_RAW);
    TEST("test_offset_from_report_numbers", offset == 946698071);

    time_t abs_last = radex_journal_calib_abs(SUM_TIME_RAW, offset);
    TEST("test_last_record_maps_to_board_now", abs_last == BOARD_NOW);

    time_t abs_prev = radex_journal_calib_abs(REC2_TIME_RAW, offset);
    TEST("test_prev_record_600s_earlier", abs_last - abs_prev == 600);
    TEST("test_delta_stays_multiple_of_600", (long)(abs_last - abs_prev) % 600 == 0);

    /* #RADEX-293: пустая история в окне — НИКОГДА не "покрыто". Это и есть
       реальный пробел, который дедуп не имеет права проглотить молча. */
    TEST("test_empty_history_never_covered",
         !radex_journal_dedup_covered(NULL, 0, 1000000, 180));

    /* Боевого вида история: шаг ~600 с с дрожанием ±25 с (реальные данные платы
       не дрожат идеально), затем реальный пробел ~67 мин (потеря Wi-Fi/перезагрузка). */
    time_t existing[] = {1000000, 1000610, 1001185, 1001830, 1002395,
                         /* пробел */ 1006395, 1006990};
    size_t n = sizeof(existing) / sizeof(existing[0]);

    /* Обе проверки покрытия — ОДНИМ тестом: мутация «порог дедупликации 0»
       (см. журнал_calib.h) обязана красить РОВНО ОДИН тест этого набора. */
    TEST("test_candidate_covered_inside_and_at_window_edge",
         radex_journal_dedup_covered(existing, n, 1000650, 180) &&
         radex_journal_dedup_covered(existing, n, 1000610 + 180, 180));
    TEST("test_candidate_just_outside_window_is_new",
         !radex_journal_dedup_covered(existing, n, 1000610 + 181, 180));
    TEST("test_candidate_inside_real_gap_is_new",
         !radex_journal_dedup_covered(existing, n, 1004000, 180));
    TEST("test_candidate_near_gap_edge_is_new",
         !radex_journal_dedup_covered(existing, n, 1002395 + 181, 180));

    /* #RADEX-293 аудит находка 2: верхняя граница валидности часов шлюза — один
       аномальный board_unix_now иначе калибрует и молча пишет до 64 точек разом. */
    TEST("test_time_valid_within_bounds",
         radex_time_valid(1700000000) && radex_time_valid(1789600000));
    TEST("test_time_valid_rejects_below_min", !radex_time_valid(1699999999));
    TEST("test_time_valid_accepts_at_max", radex_time_valid(RADON_TIME_MAX));
    TEST("test_time_valid_rejects_far_future", !radex_time_valid(RADON_TIME_MAX + 1));

    printf("итого (journal_calib): красных тестов %d из %d\n", fail_tests, total_tests);
    return fail_tests ? 1 : 0;
}
