// #USB-1: хост-тест архива с НЕСКОЛЬКИМИ сессиями и семантики 0x0BC2 (трасса RadexDC
// captures/radex_usb_04_archive2.pcap, 2 сессии; трасса 02 — 1 сессия). Векторы — test/host/radex_usb_vectors.txt,
// сделаны скриптом test/host/radex_usb_vectors.py, руками не набираются. Эталон значений — экран RadexDC
// на момент трассы 04: сессия 1 — конец 17:20:42 18.09.2026, средняя 94, 14 измерений, №1 в 15:10:43
// со скользящим средним 107; сессия 0 — конец 14:55:13, средняя 83, 264 измерения; текущие: ОА 94, средняя 94.
// Usage: test_radex_sessions <vectors.txt>; exit code = число провалов.
#include "radex_ekosf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else { printf("ok: %s\n", msg); } } while (0)

typedef struct { char name[48]; uint8_t b[1024]; size_t n; } vec_t;
static vec_t V[64];
static int nv = 0;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char line[4096];
    while (nv < 64 && fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        vec_t *v = &V[nv];
        snprintf(v->name, sizeof v->name, "%.47s", line);
        const char *h = sp + 1;
        v->n = 0;
        while (hexval(h[0]) >= 0 && hexval(h[1]) >= 0 && v->n < sizeof v->b) {
            v->b[v->n++] = (uint8_t)(hexval(h[0]) * 16 + hexval(h[1]));
            h += 2;
        }
        nv++;
    }
    fclose(f);
    return nv;
}

static const vec_t *get(const char *name)
{
    for (int i = 0; i < nv; i++) if (strcmp(V[i].name, name) == 0) return &V[i];
    printf("FAIL: нет вектора %s\n", name);
    fails++;
    exit(fails);
}

static uint16_t pnum_of(const vec_t *v) { return (uint16_t)(v->b[6] | (v->b[7] << 8)); }

static const uint8_t *result_of(const char *name, uint16_t rpc, size_t *rl)
{
    const vec_t *v = get(name);
    const uint8_t *res = NULL;
    size_t fl = 0;
    if (ekosf_parse_rsp(v->b, v->n, pnum_of(v), rpc, &res, rl, &fl) != EKOSF_RSP_OK) {
        printf("FAIL: ответ %s не принят\n", name);
        fails++;
        exit(fails);
    }
    return res;
}

int main(int argc, char **argv)
{
    if (argc < 2 || load(argv[1]) < 20) { printf("FAIL: векторы не загружены\n"); return 1; }
    uint8_t f[EKOSF_TX_MAX];
    size_t n, rl;
    const vec_t *v;
    const uint8_t *res;

    // 1. 0x0C04 несёт индекс текущей сессии: 0 при одной сессии (трасса 02), 1 при двух (трасса 04)
    v = get("t02_req_arch_begin"); n = radex_req_arch_begin(f, sizeof f, pnum_of(v), 0);
    CHECK(n == v->n && memcmp(f, v->b, n) == 0 && radex_req_allowed(f, n), "0x0C04(сессия 0) как в трассе 02");
    v = get("t04_req_arch_begin"); n = radex_req_arch_begin(f, sizeof f, pnum_of(v), 1);
    CHECK(n == v->n && memcmp(f, v->b, n) == 0 && radex_req_allowed(f, n), "0x0C04(сессия 1) как в трассе 04");
    v = get("t04_req_arch_seek"); n = radex_req_arch_seek(f, sizeof f, pnum_of(v), 0x0107);
    CHECK(n == v->n && memcmp(f, v->b, n) == 0 && radex_req_allowed(f, n), "0x0C05(263) как в трассе 04");
    n = radex_req_arch_begin(f, sizeof f, 1, 1);
    f[21] = 0x01;   // старший байт сессии != 0; CRC данных пересчитан — отказ обязан идти по смыслу
    { uint16_t dc = ekosf_checksum16(f + 12, 12); f[24] = (uint8_t)dc; f[25] = (uint8_t)(dc >> 8); }
    CHECK(!radex_req_allowed(f, n), "гейт режет 0x0C04 с сессией > 255");

    // 2. 0x0BC2: семантика полей (две трассы)
    radex_current_t c;
    res = result_of("t04_rsp_current", RADEX_RPC_CURRENT, &rl);
    CHECK(radex_parse_current(res, rl, &c), "0x0BC2 трассы 04 разобран");
    printf("   t04: last_oa=%.2f cur=%.2f avg=%.2f sko=%.2f T=%.1f RH=%.0f сессия %u/%u\n", (double)c.last_oa, (double)c.cur,
           (double)c.avg, (double)c.sko, (double)c.temp_c, (double)c.rh, (unsigned)c.session, (unsigned)c.sessions);
    CHECK(fabsf(c.last_oa - 118.0f) < 0.01f, "@12 = ОА последней записи архива (№14 сессии 1: 118.0)");
    CHECK(fabsf(c.cur - 93.66f) < 0.01f && fabsf(c.avg - 93.66f) < 0.01f, "скользящее 93.66 и средняя 93.66 (экран: ОА 94, средняя 94)");
    CHECK(fabsf(c.sko - 20.09f) < 0.01f, "@4 = СКО средней (= СКО сессии 1 в 0x0C0D)");
    CHECK(c.session == 1 && c.sessions == 2, "@76 текущая сессия 1, @74 сессий 2");
    CHECK(fabsf(c.rh - 47.0f) < 0.01f && c.temp_c > 26.0f && c.temp_c < 27.0f, "T 26.x, RH 47 (экран: 26 °C, 47 %)");
    res = result_of("t02_rsp_current", RADEX_RPC_CURRENT, &rl);
    CHECK(radex_parse_current(res, rl, &c), "0x0BC2 трассы 02 разобран");
    CHECK(fabsf(c.last_oa - 97.33f) < 0.01f && fabsf(c.cur - 79.31f) < 0.01f, "трасса 02: @12 = ОА №264 (97.33), @20 = её скользящее (79.31)");
    CHECK(c.session == 0 && c.sessions == 1, "трасса 02: сессия 0 из 1");

    // 3. Заголовок архива
    radex_arch_hdr_t h;
    res = result_of("t04_rsp_arch_hdr", RADEX_RPC_ARCH_HDR, &rl);
    CHECK(radex_parse_arch_hdr(res, rl, &h), "заголовок трассы 04 разобран");
    CHECK(h.last_gidx == 277 && h.cur_session == 1 && h.n_sess == 2 && h.chain_ok, "сквозной индекс 277, текущая сессия 1, сессий 2, цепочка до конца");
    CHECK(h.sess[0].count == 14 && h.sess[0].end_s2000 == radex_s2000(2026, 9, 18, 17, 20, 42) && fabsf(h.sess[0].avg - 93.66f) < 0.01f,
          "сессия 1: 14 записей, конец 17:20:42, средняя 93.66 (экран: 94)");
    CHECK(h.sess[0].prev_gidx == 263 && h.sess[0].prev_session == 0, "сессия 1 ссылается на сессию 0, её последняя запись — сквозной 263");
    CHECK(h.sess[1].count == 264 && h.sess[1].end_s2000 == radex_s2000(2026, 9, 18, 14, 55, 13) && fabsf(h.sess[1].avg - 82.55f) < 0.01f
          && h.sess[1].prev_gidx == 0xFFFF, "сессия 0: 264 записи, конец 14:55:13, средняя 82.55 (экран: 83), самая старая");
    CHECK((uint32_t)h.sess[0].count + h.sess[1].count == (uint32_t)h.last_gidx + 1, "сумма записей сессий = сквозной индекс + 1");
    res = result_of("t02_rsp_arch_hdr", RADEX_RPC_ARCH_HDR, &rl);
    CHECK(radex_parse_arch_hdr(res, rl, &h) && h.n_sess == 1 && h.chain_ok && h.sess[0].count == 264 && h.last_gidx == 263 && h.cur_session == 0,
          "трасса 02: одна сессия, 264 записи, сквозной 263");

    // 4. Страница после 0x0C05(277): сессия 1 №14..1, затем сразу сессия 0 №264..
    radex_arch_rec_t r[RADEX_ARCH_PAGE_RECS];
    res = result_of("t04_rsp_arch_page_last", RADEX_RPC_ARCH_PAGE, &rl);
    int k = radex_parse_arch_page(res, rl, r, RADEX_ARCH_PAGE_RECS);
    CHECK(k == 22, "в странице 22 записи");
    CHECK(k == 22 && r[0].raw0 == 1 && r[0].idx == 14 && fabsf(r[0].oa - 118.0f) < 0.01f, "первая: сессия 1 №14, ОА 118.0");
    CHECK(k == 22 && r[13].raw0 == 1 && r[13].idx == 1 && r[13].time_s2000 == radex_s2000(2026, 9, 18, 15, 10, 43)
          && fabsf(r[13].avg - 106.67f) < 0.01f, "сессия 1 №1: 15:10:43, скользящее 106.67 (экран: 107)");
    CHECK(k == 22 && r[14].raw0 == 0 && r[14].idx == 264 && r[14].time_s2000 == radex_s2000(2026, 9, 18, 14, 55, 13),
          "за ней сразу сессия 0 №264 (14:55:13) — страницы идут сквозь сессии");

    printf(fails ? "\nПРОВАЛОВ: %d\n" : "\nвсе проверки прошли\n", fails);
    return fails;
}
