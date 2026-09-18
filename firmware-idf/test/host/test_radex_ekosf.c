// #USB-1: хост-тест radex_ekosf.c на кадрах из трасс RadexDC (живой MR107ion, 2026-09-18).
// Эталонные байты НЕ набираются руками: test/host/radex_usb_vectors.txt сделан скриптом
// test/host/radex_usb_vectors.py из captures/radex_usb_02_archive.pcap и radex_usb_03_settime.pcap.
// Usage: test_radex_ekosf <vectors.txt>; exit code = число провалов.
#include "radex_ekosf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else { printf("ok: %s\n", msg); } } while (0)

typedef struct { char name[32]; uint8_t b[1024]; size_t n; } vec_t;
static vec_t V[16];
static int nv = 0;

static int hexval(int c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }

// Строки файла: "<имя> <hex>". Возврат: число векторов.
static int load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    static char line[4096];
    while (nv < 16 && fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' '); if (!sp) continue;
        *sp = 0; vec_t *v = &V[nv];
        snprintf(v->name, sizeof v->name, "%.31s", line);
        const char *h = sp + 1; v->n = 0;
        while (hexval(h[0]) >= 0 && hexval(h[1]) >= 0 && v->n < sizeof v->b) { v->b[v->n++] = (uint8_t)(hexval(h[0]) * 16 + hexval(h[1])); h += 2; }
        nv++;
    }
    fclose(f); return nv;
}

static const vec_t *get(const char *name)
{
    for (int i = 0; i < nv; i++) if (strcmp(V[i].name, name) == 0) return &V[i];
    printf("FAIL: нет вектора %s\n", name); fails++; exit(fails);
}

static uint16_t pnum_of(const vec_t *v) { return (uint16_t)(v->b[6] | (v->b[7] << 8)); }

// Построенный кадр обязан совпасть с кадром RadexDC байт-в-байт (packetNum берём из трассы).
static void check_same(const char *what, const uint8_t *f, size_t n, const vec_t *v)
{
    CHECK(n == v->n && memcmp(f, v->b, n) == 0, what);
    CHECK(radex_req_allowed(f, n), "гейт пропускает кадр из трассы");
}

// Кадр RPC с произвольным rpcId и ВЕРНЫМИ CRC — для проверки, что гейт режет по смыслу, а не по CRC.
static size_t forge(uint8_t *o, uint16_t rpc, uint16_t sig, const uint8_t *p, size_t pl)
{
    size_t dl = 4 + pl;
    o[0] = 0x7B; o[1] = 0xFF; o[2] = 0x20; o[3] = 0x00; o[4] = (uint8_t)(dl + 2); o[5] = 0; o[6] = 1; o[7] = 0; o[8] = 0; o[9] = 0;
    uint16_t c = ekosf_checksum16(o, 10); o[10] = (uint8_t)c; o[11] = (uint8_t)(c >> 8);
    o[12] = (uint8_t)rpc; o[13] = (uint8_t)(rpc >> 8); o[14] = (uint8_t)sig; o[15] = (uint8_t)(sig >> 8);
    if (pl) memcpy(o + 16, p, pl);
    c = ekosf_checksum16(o + 12, dl); o[12 + dl] = (uint8_t)c; o[13 + dl] = (uint8_t)(c >> 8);
    return 12 + dl + 2;
}

int main(int argc, char **argv)
{
    if (argc < 2 || load(argv[1]) < 12) { printf("FAIL: векторы не загружены\n"); return 1; }
    uint8_t f[EKOSF_TX_MAX]; size_t n;
    const vec_t *v;

    // 1. Запросы — байт-в-байт как у RadexDC
    v = get("req_current");    n = radex_req_current(f, sizeof f, pnum_of(v));        check_same("0x0BC2 как в трассе", f, n, v);
    v = get("req_arch_begin"); n = radex_req_arch_begin(f, sizeof f, pnum_of(v));     check_same("0x0C04 как в трассе", f, n, v);
    v = get("req_arch_hdr");   n = radex_req_arch_hdr(f, sizeof f, pnum_of(v));       check_same("0x0C0D как в трассе", f, n, v);
    v = get("req_arch_seek");  n = radex_req_arch_seek(f, sizeof f, pnum_of(v), 0x0107); check_same("0x0C05 (last_idx=0x0107) как в трассе", f, n, v);
    v = get("req_arch_page");  n = radex_req_arch_page(f, sizeof f, pnum_of(v));      check_same("0x0C0E как в трассе", f, n, v);
    v = get("req_set_time");   n = radex_req_set_time(f, sizeof f, pnum_of(v), 843058843326ULL); check_same("0x0006 как в трассе", f, n, v);

    // 2. Календарь прибора
    CHECK(radex_ms2000(2026, 9, 18, 15, 0, 43, 326) == 843058843326ULL, "ms2000: 15:00:43.326 18.09.2026 = значению из трассы 0x0006");
    CHECK(radex_s2000(2026, 9, 18, 14, 55, 13) == 843058513UL, "s2000: 14:55:13 18.09.2026 = времени записи №264");

    // 3. Гейт: запрещённые RPC не проходят даже с верными CRC
    static const uint16_t bad[] = { 8, 9, 10, 14, 15, 23, 24, 25, 0x0001, 0x0800 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        n = forge(f, bad[i], 12, NULL, 0);
        char m[64]; snprintf(m, sizeof m, "гейт режет rpc %u", (unsigned)bad[i]);
        CHECK(!radex_req_allowed(f, n), m);
    }
    { uint8_t p[8] = {0x04,0,0,0, 0,0, 0x02,0}; n = forge(f, RADEX_RPC_ARCH_BEGIN, 14, p, 8); CHECK(!radex_req_allowed(f, n), "гейт режет 0x0C04 с чужими параметрами"); }
    { uint8_t p[8] = {0x04,0,0,0, 7,1, 0x01,0}; n = forge(f, RADEX_RPC_ARCH_SEEK, 12, p, 8); CHECK(!radex_req_allowed(f, n), "гейт режет 0x0C05 с чужой сигнатурой"); }
    n = forge(f, RADEX_RPC_CURRENT, 12, NULL, 0); CHECK(radex_req_allowed(f, n), "гейт пропускает поддельный, но допустимый 0x0BC2");
    f[1] = 0x01; CHECK(!radex_req_allowed(f, n), "гейт режет чужой адрес");
    n = forge(f, RADEX_RPC_CURRENT, 12, NULL, 0); f[n - 1] ^= 0xFF; CHECK(!radex_req_allowed(f, n), "гейт режет битую CRC данных");
    CHECK(radex_req_set_time(f, sizeof f, 1, 0) == 0, "время 2000-01-01 не строится (вне окна)");
    CHECK(radex_req_set_time(f, sizeof f, 1, radex_ms2000(2050, 1, 1, 0, 0, 0, 0)) == 0, "время 2050 не строится (вне окна)");

    // 4. Ответы
    const uint8_t *res; size_t rl, fl;
    v = get("rsp_arch_begin");
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_ARCH_BEGIN, &res, &rl, &fl) == EKOSF_RSP_OK && rl == 0 && fl == v->n, "ack 0x0C04 принят, результат пуст");
    CHECK(ekosf_parse_rsp(v->b, v->n - 1, pnum_of(v), RADEX_RPC_ARCH_BEGIN, &res, &rl, &fl) == EKOSF_RSP_NEED_MORE, "неполный кадр — ждать ещё");
    CHECK(ekosf_parse_rsp(v->b, v->n, (uint16_t)(pnum_of(v) + 1), RADEX_RPC_ARCH_BEGIN, &res, &rl, &fl) == EKOSF_RSP_MISMATCH, "чужой packetNum отвергнут");
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_ARCH_SEEK, &res, &rl, &fl) == EKOSF_RSP_MISMATCH, "чужой rpcId отвергнут");
    { uint8_t t[64]; memcpy(t, v->b, v->n); t[13] ^= 0x01; CHECK(ekosf_parse_rsp(t, v->n, pnum_of(v), RADEX_RPC_ARCH_BEGIN, &res, &rl, &fl) == EKOSF_RSP_BAD_CRC, "битые данные отвергнуты"); }
    { uint8_t t[64]; memcpy(t, v->b, v->n); t[0] = 0x7B; CHECK(ekosf_parse_rsp(t, v->n, pnum_of(v), RADEX_RPC_ARCH_BEGIN, &res, &rl, &fl) == EKOSF_RSP_BAD_HDR, "не ответ (0x7B) отвергнут"); }

    v = get("rsp_set_time");
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_SET_TIME, &res, &rl, &fl) == EKOSF_RSP_OK && rl == 2 && res[0] == 1 && res[1] == 0, "ответ 0x0006 = 0100 (успех)");

    v = get("rsp_current");
    radex_current_t c;
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_CURRENT, &res, &rl, &fl) == EKOSF_RSP_OK && radex_parse_current(res, rl, &c), "0x0BC2 разобран");
    printf("   0x0BC2: avg=%.2f cur=%.2f T=%.1f RH=%.0f %02u:%02u:%02u %02u.%02u.%02u\n", (double)c.avg, (double)c.cur, (double)c.temp_c, (double)c.rh,
           (unsigned)c.hh, (unsigned)c.mm, (unsigned)c.ss, (unsigned)c.dd, (unsigned)c.mo, (unsigned)c.yy);
    CHECK(fabsf(c.avg - 82.55f) < 0.01f, "средняя ОА 82.55 (экран RadexDC: 82-83)");
    CHECK(fabsf(c.cur - 79.31f) < 0.01f, "текущая ОА 79.31 (экран оператора: ~79, STATUS #USB-1)");
    CHECK(c.temp_c > 26.0f && c.temp_c < 26.5f && fabsf(c.rh - 47.0f) < 0.01f, "T 26.x °C, RH 47 % (экран: 26 °C, 47 %)");
    CHECK(c.dd == 18 && c.mo == 9 && c.yy == 26 && c.hh == 14 && c.clock_ok, "часы прибора 18.09.26 14 ч");

    v = get("rsp_arch_hdr");
    radex_arch_hdr_t h;
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_ARCH_HDR, &res, &rl, &fl) == EKOSF_RSP_OK && radex_parse_arch_hdr(res, rl, &h), "заголовок архива разобран");
    CHECK(h.last_idx == 0x0107 && h.last_record == 264 && h.time_s2000 == 843058513UL, "заголовок: last_idx 263, запись №264, 14:55:13");

    v = get("rsp_arch_page");
    radex_arch_rec_t r[RADEX_ARCH_PAGE_RECS];
    int k = -1;
    CHECK(ekosf_parse_rsp(v->b, v->n, pnum_of(v), RADEX_RPC_ARCH_PAGE, &res, &rl, &fl) == EKOSF_RSP_OK, "страница архива принята");
    k = radex_parse_arch_page(res, rl, r, RADEX_ARCH_PAGE_RECS);
    CHECK(k == 22, "в странице 22 записи");
    CHECK(k == 22 && r[0].idx == 264 && r[0].time_s2000 == 843058513UL && r[0].t_x10 == 262 && r[0].rh == 47 && r[0].flags == 0x10, "запись №264: 14:55:13, T 26.2, RH 47, флаги 0x10");
    CHECK(k == 22 && fabsf(r[0].avg - 79.31f) < 0.01f, "запись №264: скользящее среднее 79.3 (экран RadexDC: 79)");
    int desc = 1; for (int i = 1; i < k; i++) if (r[i].idx != r[i - 1].idx - 1 || r[i - 1].time_s2000 - r[i].time_s2000 < 599 || r[i - 1].time_s2000 - r[i].time_s2000 > 601) desc = 0;   /* в трассе один шаг 599 с (№254->253) */
    CHECK(k == 22 && desc && r[21].idx == 243, "номера по убыванию подряд, шаг 600±1 с, последняя №243");

    printf(fails ? "\nПРОВАЛОВ: %d\n" : "\nвсе проверки прошли\n", fails);
    return fails;
}
