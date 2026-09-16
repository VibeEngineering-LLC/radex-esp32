/*#RADEX-281: хост-тест разбора журнала на РЕАЛЬНЫХ байтах захвата 2026-09-16 (Захват 3). Сборка: test/host/run.ps1*/
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "radex_journal_parse.h"

static int g_fail_tests = 0;
static int g_cur_fail;
static int g_total_tests = 0;

/* Дословно из отчёта (Захват 3, t=56.73 / 56.85 / 59.97 / 60.09). */
static const uint8_t SUM1[20] = {0x00,0x00,0x02,0x00,0x00,0x00,0x03,0x00,0xa9,0xa9,0x3d,0x32,0x9b,0x6c,0xfa,0x42,0x07,0x1e,0x40,0x42};
static const uint8_t SUM2[20] = {0x01,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t REC3[24] = {0x02,0x00,0x00,0x00,0x03,0x00,0xa9,0xa9,0x3d,0x32,0x56,0x55,0x9d,0x42,0x9b,0x6c,0xfa,0x42,0x07,0x01,0x00,0x00,0x2a,0x10};
static const uint8_t REC2[24] = {0x01,0x00,0x00,0x00,0x02,0x00,0x51,0xa7,0x3d,0x32,0xd3,0x45,0xf7,0x42,0x1f,0x7c,0x14,0x43,0x08,0x01,0x00,0x00,0x2a,0x10};

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_cur_fail++; \
        printf("    FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

#define RUN(fn) do { \
    g_cur_fail = 0; \
    g_total_tests++; \
    fn(); \
    printf("%s %s\n", g_cur_fail ? "RED  " : "GREEN", #fn); \
    if (g_cur_fail) g_fail_tests++; \
} while(0)

static int feq(float a, float b, float tol) {
    return fabs((double)a - (double)b) <= (double)tol;
}

static void test_summary_last_record(void) {
    radex_journal_summary_t s;
    int r = radex_journal_parse_summary(SUM1, 20, &s);
    CHECK(r == RADEX_J_OK, "parse_summary returned %d", r);
    CHECK(s.last_record == 3, "last_record = %u", s.last_record);
    CHECK(s.seq == 0, "seq = %u", s.seq);
    CHECK(s.raw2 == 2, "raw2 = %u", s.raw2);
    CHECK(s.raw4 == 0, "raw4 = %u", s.raw4);
    CHECK(s.time_raw == 0x323da9a9, "time_raw = 0x%08lx", (unsigned long)s.time_raw);
}

static void test_summary_raw_floats(void) {
    radex_journal_summary_t s;
    int r = radex_journal_parse_summary(SUM1, 20, &s);
    CHECK(r == RADEX_J_OK, "parse_summary returned %d", r);
    CHECK(feq(s.avg, 125.21f, 0.01f), "avg = %.2f", s.avg);
    CHECK(feq(s.sko, 48.03f, 0.01f), "sko = %.2f", s.sko);
}

static void test_summary_filler(void) {
    radex_journal_summary_t s;
    int r = radex_journal_parse_summary(SUM2, 20, &s);
    CHECK(r == RADEX_J_FILLER, "parse_summary returned %d", r);
    CHECK(s.seq == 1, "seq = %u", s.seq);
}

static void test_record_number(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 24, &r);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(r.number == 3, "number = %u", r.number);
    CHECK(r.seq == 2, "seq = %u", r.seq);

    int r2 = radex_journal_parse_record(REC2, 24, &r);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(r.number == 2, "number = %u", r.number);
    CHECK(r.seq == 1, "seq = %u", r.seq);
}

static void test_record_oa(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 24, &r);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(feq(r.oa, 125.21f, 0.01f), "oa = %.2f", r.oa);

    int r2 = radex_journal_parse_record(REC2, 24, &r);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(feq(r.oa, 148.49f, 0.01f), "oa = %.2f", r.oa);
}

static void test_record_temp(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 24, &r);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(r.temp_x10 == 263, "temp_x10 = %u", r.temp_x10);

    int r2 = radex_journal_parse_record(REC2, 24, &r);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(r.temp_x10 == 264, "temp_x10 = %u", r.temp_x10);
}

static void test_record_humidity(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 24, &r);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(r.humidity == 42, "humidity = %u", r.humidity);

    int r2 = radex_journal_parse_record(REC2, 24, &r);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(r.humidity == 42, "humidity = %u", r.humidity);
}

static void test_record_time(void) {
    radex_journal_record_t a, b;
    int r1 = radex_journal_parse_record(REC3, 24, &a);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(a.time_raw == 0x323da9a9, "time_raw = 0x%08lx", (unsigned long)a.time_raw);

    int r2 = radex_journal_parse_record(REC2, 24, &b);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(b.time_raw == 0x323da751, "time_raw = 0x%08lx", (unsigned long)b.time_raw);

    CHECK((a.time_raw - b.time_raw) == 600, "difference = %lu", (unsigned long)(a.time_raw - b.time_raw));
}

static void test_record_raw_unknown(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 24, &r);
    CHECK(r1 == RADEX_J_OK, "parse_record REC3 returned %d", r1);
    CHECK(feq(r.unk_f10, 78.67f, 0.01f), "unk_f10 = %.2f", r.unk_f10);
    CHECK(r.raw2 == 0, "raw2 = %u", r.raw2);
    CHECK(r.raw20 == 0, "raw20 = %u", r.raw20);
    CHECK(r.flags == 0x10, "flags = 0x%02x", r.flags);

    int r2 = radex_journal_parse_record(REC2, 24, &r);
    CHECK(r2 == RADEX_J_OK, "parse_record REC2 returned %d", r2);
    CHECK(feq(r.unk_f10, 123.64f, 0.01f), "unk_f10 = %.2f", r.unk_f10);
    CHECK(r.raw20 == 0, "raw20 = %u", r.raw20);
    CHECK(r.flags == 0x10, "flags = 0x%02x", r.flags);
}

static void test_record_short(void) {
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(REC3, 20, &r);
    CHECK(r1 == RADEX_J_SHORT, "parse_record REC3 with len=20 returned %d", r1);

    int r2 = radex_journal_parse_record(NULL, 24, &r);
    CHECK(r2 == RADEX_J_BADARG, "parse_record NULL returned %d", r2);

    radex_journal_summary_t s;
    int r3 = radex_journal_parse_summary(SUM1, 19, &s);
    CHECK(r3 == RADEX_J_SHORT, "parse_summary SUM1 with len=19 returned %d", r3);
}

static void test_record_filler(void) {
    uint8_t filler[24] = {0x01, 0x00};
    memset(filler + 2, 0xff, 22);
    radex_journal_record_t r;
    int r1 = radex_journal_parse_record(filler, 24, &r);
    CHECK(r1 == RADEX_J_FILLER, "parse_record filler returned %d", r1);
}

static void test_whitelist_exact(void) {
    CHECK(radex_journal_cmd_allowed(RADEX_J_CMD_SUMMARY, 8), "RADEX_J_CMD_SUMMARY allowed");
    CHECK(radex_journal_cmd_allowed(RADEX_J_CMD_SUMMARY_NEXT, 8), "RADEX_J_CMD_SUMMARY_NEXT allowed");
    CHECK(radex_journal_cmd_allowed(RADEX_J_CMD_RECORDS, 8), "RADEX_J_CMD_RECORDS allowed");
    CHECK(radex_journal_cmd_allowed(RADEX_J_CMD_RECORDS_NEXT, 8), "RADEX_J_CMD_RECORDS_NEXT allowed");

    uint8_t cmd1[8] = {0x47, 0, 0, 0, 0, 0, 0, 0};
    CHECK(radex_journal_cmd_allowed(cmd1, 8), "cmd1 allowed");

    uint8_t cmd2[8] = {0x81, 0xff, 0, 0, 0, 0, 0, 0};
    CHECK(radex_journal_cmd_allowed(cmd2, 8), "cmd2 allowed");

    uint8_t cmd3[8] = {0x48, 0, 2, 0, 1, 0, 0, 0};
    CHECK(radex_journal_cmd_allowed(cmd3, 8), "cmd3 allowed");

    uint8_t cmd4[8] = {0x82, 0xff, 0, 0, 0, 0, 0, 0};
    CHECK(radex_journal_cmd_allowed(cmd4, 8), "cmd4 allowed");
}

static void test_whitelist_rejects(void) {
    CHECK(!radex_journal_cmd_allowed(NULL, 8), "NULL/8 not allowed");
    CHECK(!radex_journal_cmd_allowed(RADEX_J_CMD_SUMMARY, 7), "SUMMARY with len=7 not allowed");

    uint8_t cmd1[9] = {0x47, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!radex_journal_cmd_allowed(cmd1, 9), "9-byte array starting with SUMMARY bytes not allowed");

    uint8_t cmd2[8] = {0x48, 0, 3, 0, 1, 0, 0, 0};
    CHECK(!radex_journal_cmd_allowed(cmd2, 8), "cmd2 not allowed");

    uint8_t cmd3[8] = {0x47, 1, 0, 0, 0, 0, 0, 0};
    CHECK(!radex_journal_cmd_allowed(cmd3, 8), "cmd3 not allowed");

    uint8_t cmd4[8] = {0x81, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!radex_journal_cmd_allowed(cmd4, 8), "cmd4 not allowed");

    uint8_t cmd5[8] = {0x82, 0xff, 0, 0, 0, 0, 0, 1};
    CHECK(!radex_journal_cmd_allowed(cmd5, 8), "cmd5 not allowed");

    int count = 0;
    for (int b = 0; b < 256; ++b) {
        uint8_t cmd[8] = {(uint8_t)b, 0, 0, 0, 0, 0, 0, 0};
        if (radex_journal_cmd_allowed(cmd, 8)) {
            count++;
            CHECK(b == 0x47, "only 0x47 allowed in loop, got %02x", b);
        }
    }
    CHECK(count == 1, "count = %d", count);

    count = 0;
    for (int b = 0; b < 256; ++b) {
        uint8_t cmd[8] = {(uint8_t)b, 0xff, 0, 0, 0, 0, 0, 0};
        if (radex_journal_cmd_allowed(cmd, 8)) {
            count++;
            CHECK((b == 0x81 || b == 0x82), "only 0x81 and 0x82 allowed in loop, got %02x", b);
        }
    }
    CHECK(count == 2, "count = %d", count);
}

static void test_cccd(void) {
    uint8_t cccd1[2] = {1, 0};
    CHECK(radex_journal_cccd_allowed(cccd1, 2), "cccd1 allowed");

    uint8_t cccd2[2] = {0, 0};
    CHECK(radex_journal_cccd_allowed(cccd2, 2), "cccd2 allowed");

    uint8_t cccd3[2] = {2, 0};
    CHECK(!radex_journal_cccd_allowed(cccd3, 2), "cccd3 not allowed");

    uint8_t cccd4[3] = {1, 0, 0};
    CHECK(!radex_journal_cccd_allowed(cccd4, 3), "cccd4 not allowed");

    CHECK(!radex_journal_cccd_allowed(NULL, 2), "NULL/2 not allowed");
}

static void test_json_shape(void) {
    radex_journal_t j;
    memset(&j, 0, sizeof(j));
    j.valid = true;
    j.ok = true;
    strcpy(j.status, "ok");
    j.finished_s = 42;
    j.mtu = 247;
    j.have_summary = true;
    j.summary.last_record = 3;
    j.summary.avg = 125.25f;
    j.n_records = 1;
    j.records[0].number = 3;
    j.records[0].oa = 148.5f;
    j.records[0].temp_x10 = 264;
    j.records[0].humidity = 42;
    j.records[0].flags = 16;
    j.n_record_pkt = 1;
    radex_journal_raw_store(&j.record_pkt[0], REC2, 24);

    char buf[2048];
    int n = radex_journal_json(&j, false, true, buf, sizeof(buf));
    CHECK(n > 0, "json returned %d", n);
    CHECK(strstr(buf, "\"pending\":true"), "contains pending");
    CHECK(strstr(buf, "\"mtu\":247"), "contains mtu");
    CHECK(strstr(buf, "\"last_record\":3"), "contains last_record");
    CHECK(strstr(buf, "\"avg\":125.25"), "contains avg");
    CHECK(strstr(buf, "\"number\":3"), "contains number");
    CHECK(strstr(buf, "\"oa\":148.50"), "contains oa");
    CHECK(strstr(buf, "\"t_x10\":264"), "contains t_x10");
    CHECK(strstr(buf, "\"humidity\":42"), "contains humidity");
    CHECK(strstr(buf, "\"flags\":16"), "contains flags");
    CHECK(strstr(buf, "\"record_packets\":[\"01000000020051a73d32d345f7421f7c1443080100002a10\"]"), "contains record_packets");
    CHECK(strstr(buf, "\"summary\":{"), "contains summary");

    strcpy(j.status, "a\"b");
    n = radex_journal_json(&j, false, true, buf, sizeof(buf));
    CHECK(n > 0, "json returned %d", n);
    CHECK(strstr(buf, "\"status\":\"ab\""), "contains escaped status");

    j.have_summary = false;
    n = radex_journal_json(&j, false, true, buf, sizeof(buf));
    CHECK(n > 0, "json returned %d", n);
    CHECK(strstr(buf, "\"summary\":null"), "contains null summary");

    n = radex_journal_json(&j, false, false, buf, 16);
    CHECK(n == -1, "buffer too small returned %d", n);
}

static void test_raw_store(void) {
    uint8_t data[40];
    for (int i = 0; i < 40; ++i)
        data[i] = i;
    radex_journal_raw_t r;
    radex_journal_raw_store(&r, data, 40);
    CHECK(r.len == 32, "len = %u", r.len);
    CHECK(r.orig_len == 40, "orig_len = %u", r.orig_len);
    CHECK(r.data[31] == 31, "data[31] = %u", r.data[31]);
}

int main(void) {
    RUN(test_summary_last_record);
    RUN(test_summary_raw_floats);
    RUN(test_summary_filler);
    RUN(test_record_number);
    RUN(test_record_oa);
    RUN(test_record_temp);
    RUN(test_record_humidity);
    RUN(test_record_time);
    RUN(test_record_raw_unknown);
    RUN(test_record_short);
    RUN(test_record_filler);
    RUN(test_whitelist_exact);
    RUN(test_whitelist_rejects);
    RUN(test_cccd);
    RUN(test_json_shape);
    RUN(test_raw_store);

    printf("итого: красных тестов %d из %d\n", g_fail_tests, g_total_tests);
    return g_fail_tests ? 1 : 0;
}
