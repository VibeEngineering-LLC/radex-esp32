/* #RADEX-281 (аудит 281-B F2/F3/F5): хост-тест привязки квитанций и notify к шагу. Сборка: test/host/run.ps1 */
#include <stdio.h>
#include <string.h>
#include "radex_journal_parse.h"
#include "radex_journal_track.h"

static int g_fail_tests = 0;
static int g_cur_fail;
static int g_total_tests = 0;

#define CHECK(cond, ...) do { if (!(cond)) { g_cur_fail++; printf("    FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define RUN(fn) do { g_cur_fail = 0; g_total_tests++; fn(); printf("%s %s\n", g_cur_fail ? "RED  " : "GREEN", #fn); if (g_cur_fail) g_fail_tests++; } while (0)

static const uint8_t REC3[24] = {0x02,0x00,0x00,0x00,0x03,0x00,0xa9,0xa9,0x3d,0x32,0x56,0x55,0x9d,0x42,0x9b,0x6c,0xfa,0x42,0x07,0x01,0x00,0x00,0x2a,0x10};

static void test_cccd_step(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_new_session(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_CCCD, false);
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_CCCD, 0) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(radex_j_track_step_done(&t), "Step should be done");
    CHECK(t.outstanding == 0, "Outstanding should be zero");
    CHECK(radex_j_track_can_start(&t), "Can start should be true");
}

static void test_notify_step(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, true);
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(!radex_j_track_step_done(&t), "Step should not be done yet");
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(radex_j_track_step_done(&t), "Step should be done");
}

static void test_stale_ack_drains_before_start(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, true);
    radex_j_track_new_session(&t);
    CHECK(!radex_j_track_can_start(&t), "Can start should be false");
    radex_j_track_step_idle(&t);
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_STALE, "Expected RJ_EV_STALE");
    CHECK(t.stale_acks == 1, "Stale acks should be 1");
    CHECK(t.unexpected == 0, "Unexpected should be 0");
    CHECK(radex_j_track_can_start(&t), "Can start should be true");
}

static void test_stale_ack_not_credited(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, true);
    radex_j_track_new_session(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, false);
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_STALE, "Expected RJ_EV_STALE");
    CHECK(!t.acked, "Should not be acked");
    CHECK(!radex_j_track_step_done(&t), "Step should not be done yet");
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(radex_j_track_step_done(&t), "Step should be done");
    CHECK(t.stale_acks == 1, "Stale acks should be 1");
}

static void test_late_notify_unexpected(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, false);
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_UNEXPECTED, "Expected RJ_EV_UNEXPECTED");
    CHECK(t.unexpected == 1, "Unexpected should be 1");
    radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0);
    radex_j_track_step_idle(&t);
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_UNEXPECTED, "Expected RJ_EV_UNEXPECTED during delay");
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, true);
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(t.unexpected == 2, "Unexpected should be 2");
}

static void test_double_notify(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, true);
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(radex_j_track_on_notify(&t) == RJ_EV_UNEXPECTED, "Expected RJ_EV_UNEXPECTED");
}

static void test_ack_error_status(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_RX, false);
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 3) == RJ_EV_CURRENT, "Expected RJ_EV_CURRENT");
    CHECK(t.ack_status == 3, "Ack status should be 3");
    CHECK(!radex_j_track_step_done(&t), "Step should not be done");
}

static void test_foreign_and_orphan_ack(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    CHECK(radex_j_track_on_ack(&t, 0x0049, 0) == RJ_EV_FOREIGN, "Expected RJ_EV_FOREIGN");
    CHECK(t.unexpected == 0 && t.outstanding == 0, "Unexpected and outstanding should be zero");
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_UNEXPECTED, "Expected RJ_EV_UNEXPECTED");
    CHECK(t.unexpected == 1, "Unexpected should be 1");
}

static void test_wrong_handle_ack(void) {
    radex_j_track_t t;
    radex_j_track_reset_all(&t);
    radex_j_track_on_send(&t, RADEX_NUS_H_CCCD, false);
    CHECK(radex_j_track_on_ack(&t, RADEX_NUS_H_RX, 0) == RJ_EV_UNEXPECTED, "Expected RJ_EV_UNEXPECTED");
    CHECK(!t.acked, "Should not be acked");
}

static void test_final_status(void) {
    radex_j_track_t t;
    radex_journal_t j;
    char st[48];

    // Test case a
    memset(&j, 0, sizeof(j));
    j.n_records = 1;
    j.n_record_pkt = 1;
    radex_journal_raw_store(&j.record_pkt[0], REC3, 24);
    radex_j_track_reset_all(&t);
    CHECK(radex_j_final_status(&j, &t, st, sizeof(st)), "Final status should be true");
    CHECK(strcmp(st, "ok") == 0, "Status should be 'ok'");

    // Test case b
    memset(&j, 0, sizeof(j));
    j.n_records = 0;
    radex_j_track_reset_all(&t);
    CHECK(!radex_j_final_status(&j, &t, st, sizeof(st)), "Final status should be false");
    CHECK(strstr(st, "no records"), "Should contain 'no records'");

    // Test case c
    memset(&j, 0, sizeof(j));
    j.n_records = 1;
    j.n_record_pkt = 1;
    radex_journal_raw_store(&j.record_pkt[0], REC3, 20);
    radex_j_track_reset_all(&t);
    CHECK(!radex_j_final_status(&j, &t, st, sizeof(st)), "Final status should be false");
    CHECK(strstr(st, "short packet"), "Should contain 'short packet'");

    // Test case d
    memset(&j, 0, sizeof(j));
    j.n_records = 1;
    j.n_record_pkt = 1;
    radex_journal_raw_store(&j.record_pkt[0], REC3, 24);
    radex_j_track_reset_all(&t);
    t.unexpected = 1;
    CHECK(!radex_j_final_status(&j, &t, st, sizeof(st)), "Final status should be false");
    CHECK(strstr(st, "unexpected"), "Should contain 'unexpected'");
}

int main(void) {
    RUN(test_cccd_step);
    RUN(test_notify_step);
    RUN(test_stale_ack_drains_before_start);
    RUN(test_stale_ack_not_credited);
    RUN(test_late_notify_unexpected);
    RUN(test_double_notify);
    RUN(test_ack_error_status);
    RUN(test_foreign_and_orphan_ack);
    RUN(test_wrong_handle_ack);
    RUN(test_final_status);

    printf("итого (track): красных тестов %d из %d\n", g_fail_tests, g_total_tests);
    return g_fail_tests ? 1 : 0;
}
