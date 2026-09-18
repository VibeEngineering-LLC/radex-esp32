// Radex MR107ion по USB: кадр EKOSF + RPC (см. radex_ekosf.h). Порт эталона ПК
// scripts/radex_usb/radex_ekosf.py (build/checksum16/rpc_exec), байты сверены с трассами #USB-1.
#include "radex_ekosf.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static float    rdf(const uint8_t *p)  { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static void     wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

uint16_t ekosf_checksum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0; size_t i = 0;
    while (i + 1 < n) { s += (uint32_t)(p[i] | ((uint16_t)p[i+1] << 8)); i += 2; }
    if (i < n) s += p[i];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s & 0xFFFF);
}

// Кадр: [ft][addr][cmd u16][dataLen u16][packetNum u16][reserved u16][crc u16] + data + dataCRC;
// dataLen = len(data) + 2. data = [rpcId u16][rpcSig u16][params].
static size_t build_rpc(uint8_t *out, size_t cap, uint16_t pnum, uint16_t rpc, uint16_t sig,
                        const uint8_t *params, size_t plen)
{
    size_t dl = 4 + plen;
    size_t total = EKOSF_HDR_LEN + dl + 2;
    if (out == NULL || total > cap) return 0;
    out[0] = EKOSF_FT_MASTER; out[1] = EKOSF_ADDR;
    wr16(out + 2, EKOSF_CMD_RPC); wr16(out + 4, (uint16_t)(dl + 2)); wr16(out + 6, pnum); wr16(out + 8, 0);
    wr16(out + 10, ekosf_checksum16(out, 10));
    wr16(out + 12, rpc); wr16(out + 14, sig);
    if (plen) memcpy(out + 16, params, plen);
    wr16(out + 12 + dl, ekosf_checksum16(out + 12, dl));
    return total;
}

// Общая форма параметров 0x0C04/0x0C05 — байт-в-байт из трасс 02 и 04: 04000000 <u16> 0100.
static const uint8_t P_ARCH_BEGIN[8] = {0x04,0x00,0x00,0x00, 0x00,0x00, 0x01,0x00};

size_t radex_req_current(uint8_t *out, size_t cap, uint16_t pnum) { return build_rpc(out, cap, pnum, RADEX_RPC_CURRENT, RADEX_SIG_READ, NULL, 0); }
// 0x0C04: u16 = индекс текущей сессии (трасса 02: 0000 при одной сессии, трасса 04: 0100 при двух).
size_t radex_req_arch_begin(uint8_t *out, size_t cap, uint16_t pnum, uint8_t session)
{
    uint8_t p[8];
    memcpy(p, P_ARCH_BEGIN, 8);
    p[4] = session;
    return build_rpc(out, cap, pnum, RADEX_RPC_ARCH_BEGIN, RADEX_SIG_ARCH, p, 8);
}
size_t radex_req_arch_hdr(uint8_t *out, size_t cap, uint16_t pnum) { return build_rpc(out, cap, pnum, RADEX_RPC_ARCH_HDR, RADEX_SIG_READ, NULL, 0); }
size_t radex_req_arch_page(uint8_t *out, size_t cap, uint16_t pnum) { return build_rpc(out, cap, pnum, RADEX_RPC_ARCH_PAGE, RADEX_SIG_READ, NULL, 0); }

// 0x0C05: в трассе 04000000 0701 0100, где 0x0107 = last_idx из ответа 0x0C0D того же сеанса.
// Форма байт-в-байт как в трассе, меняется только это u16.
size_t radex_req_arch_seek(uint8_t *out, size_t cap, uint16_t pnum, uint16_t last_idx)
{
    uint8_t p[8] = {0x04,0x00,0x00,0x00, 0,0, 0x01,0x00};
    wr16(p + 4, last_idx);
    return build_rpc(out, cap, pnum, RADEX_RPC_ARCH_SEEK, RADEX_SIG_ARCH, p, 8);
}

// Допустимое окно для записи часов: 2023-01-01 .. 2044-01-01 (защита от мусорных часов платы).
static uint64_t time_lo(void) { return radex_ms2000(2023, 1, 1, 0, 0, 0, 0); }
static uint64_t time_hi(void) { return radex_ms2000(2044, 1, 1, 0, 0, 0, 0); }

size_t radex_req_set_time(uint8_t *out, size_t cap, uint16_t pnum, uint64_t ms2000)
{
    if (ms2000 < time_lo() || ms2000 >= time_hi()) return 0;
    uint8_t p[8];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(ms2000 >> (8 * i));
    return build_rpc(out, cap, pnum, RADEX_RPC_SET_TIME, RADEX_SIG_TIME, p, 8);
}

bool radex_req_allowed(const uint8_t *f, size_t n)
{
    if (f == NULL || n < EKOSF_HDR_LEN + 6 || n > EKOSF_HDR_LEN + 4 + 8 + 2) return false;
    if (f[0] != EKOSF_FT_MASTER || f[1] != EKOSF_ADDR) return false;
    if (rd16(f + 2) != EKOSF_CMD_RPC) return false;
    if ((uint16_t)(rd16(f + 4)) != (uint16_t)(n - EKOSF_HDR_LEN)) return false;
    if (rd16(f + 8) != 0) return false;
    if (ekosf_checksum16(f, 10) != rd16(f + 10)) return false;
    size_t dl = n - EKOSF_HDR_LEN - 2;
    const uint8_t *d = f + EKOSF_HDR_LEN;
    if (ekosf_checksum16(d, dl) != rd16(d + dl)) return false;
    uint16_t rpc = rd16(d), sig = rd16(d + 2);
    const uint8_t *p = d + 4; size_t plen = dl - 4;
    switch (rpc) {
    case RADEX_RPC_CURRENT: case RADEX_RPC_ARCH_HDR: case RADEX_RPC_ARCH_PAGE:
        return sig == RADEX_SIG_READ && plen == 0;
    case RADEX_RPC_ARCH_BEGIN:
        return sig == RADEX_SIG_ARCH && plen == 8 && memcmp(p, P_ARCH_BEGIN, 4) == 0 && p[5] == 0x00 && p[6] == 0x01 && p[7] == 0x00;
    case RADEX_RPC_ARCH_SEEK:
        return sig == RADEX_SIG_ARCH && plen == 8 && memcmp(p, P_ARCH_BEGIN, 4) == 0 && p[6] == 0x01 && p[7] == 0x00;
    case RADEX_RPC_SET_TIME: {
        if (sig != RADEX_SIG_TIME || plen != 8) return false;
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
        return v >= time_lo() && v < time_hi();
    }
    default:
        return false;   // любой иной RPC — запрещён (в т.ч. 8/9/10/14/15/23-25)
    }
}

int ekosf_parse_rsp(const uint8_t *buf, size_t len, uint16_t pnum, uint16_t rpc_id,
                    const uint8_t **res, size_t *res_len, size_t *frame_len)
{
    if (buf == NULL || len < EKOSF_HDR_LEN) return EKOSF_RSP_NEED_MORE;
    if (buf[0] != EKOSF_FT_SLAVE) return EKOSF_RSP_BAD_HDR;
    if (ekosf_checksum16(buf, 10) != rd16(buf + 10)) return EKOSF_RSP_BAD_HDR;
    uint16_t cmd = rd16(buf + 2), dlen = rd16(buf + 4), pn = rd16(buf + 6);
    if (len < (size_t)EKOSF_HDR_LEN + dlen) return EKOSF_RSP_NEED_MORE;
    if (frame_len) *frame_len = (size_t)EKOSF_HDR_LEN + dlen;
    if (cmd >= 0xFF00) return EKOSF_RSP_DEV_ERROR;
    if (cmd != EKOSF_RSP_RPC || pn != pnum || dlen < 2 + 4) return EKOSF_RSP_MISMATCH;
    const uint8_t *d = buf + EKOSF_HDR_LEN;
    size_t dl = (size_t)dlen - 2;
    if (ekosf_checksum16(d, dl) != rd16(d + dl)) return EKOSF_RSP_BAD_CRC;
    if (rd16(d) != rpc_id) return EKOSF_RSP_MISMATCH;
    if (res) *res = d + 4;
    if (res_len) *res_len = dl - 4;
    return EKOSF_RSP_OK;
}

// Раскладка — STATUS.md #USB-1; stack начинается с u32 длины блока (0x58), поля — после неё.
bool radex_parse_current(const uint8_t *res, size_t n, radex_current_t *out)
{
    if (res == NULL || out == NULL || n < 4 + RADEX_CUR_BLOCK_LEN) return false;
    if (rd32(res) < RADEX_CUR_BLOCK_LEN) return false;
    const uint8_t *d = res + 4;
    out->avg = rdf(d + 0);
    out->sko = rdf(d + 4);
    out->counter = rd32(d + 8);
    out->last_oa = rdf(d + 12);
    out->cur = rdf(d + 20);
    out->sessions = rd16(d + 74);
    out->session = rd16(d + 76);
    out->temp_c = (float)(int16_t)rd16(d + 64) / 10.0f;
    out->rh = (float)rd16(d + 68);
    out->ss = d[80]; out->mm = d[81]; out->hh = d[82]; out->dd = d[83]; out->mo = d[84]; out->yy = d[85];
    out->clock_ok = !(out->mo < 1 || out->mo > 12 || out->dd < 1 || out->dd > 31 || out->hh > 23 || out->mm > 59 || out->ss > 59);
    return true;
}

bool radex_parse_arch_hdr(const uint8_t *res, size_t n, radex_arch_hdr_t *out)
{
    if (res == NULL || out == NULL || n < 4 + 4 + RADEX_ARCH_SESS_LEN) return false;
    size_t avail = n - 4;
    if (rd32(res) < avail) avail = rd32(res);
    const uint8_t *d = res + 4;
    memset(out, 0, sizeof(*out));
    out->last_gidx = rd16(d + 0); out->cur_session = rd16(d + 2);
    for (size_t off = 4; off + RADEX_ARCH_SESS_LEN <= avail && out->n_sess < RADEX_ARCH_MAX_SESS; off += RADEX_ARCH_SESS_LEN) {
        radex_arch_sess_t *s = &out->sess[out->n_sess++];
        s->count = rd16(d + off); s->end_s2000 = rd32(d + off + 2); s->avg = rdf(d + off + 6);
        s->sko = rdf(d + off + 10); s->prev_gidx = rd16(d + off + 14); s->prev_session = rd16(d + off + 16);
        if (s->prev_gidx == 0xFFFF) { out->chain_ok = true; break; }   // самая старая сессия
    }
    return out->n_sess > 0;
}

int radex_parse_arch_page(const uint8_t *res, size_t n, radex_arch_rec_t *out, int max)
{
    if (res == NULL || out == NULL || n < 4) return -1;
    size_t avail = n - 4;
    uint32_t L = rd32(res);
    if (L < avail) avail = L;
    size_t cnt = avail / RADEX_ARCH_REC_LEN;
    int k = 0;
    for (size_t i = 0; i < cnt && k < max; i++) {
        const uint8_t *p = res + 4 + i * RADEX_ARCH_REC_LEN;
        bool filler = true;
        for (int b = 0; b < RADEX_ARCH_REC_LEN; b++) if (p[b] != 0xFF) { filler = false; break; }
        if (filler) break;
        radex_arch_rec_t *r = &out[k++];
        r->raw0 = rd16(p + 0); r->idx = rd16(p + 2); r->time_s2000 = rd32(p + 4);
        r->oa = rdf(p + 8); r->avg = rdf(p + 12); r->t_x10 = rd32(p + 16); r->rh = p[20]; r->flags = p[21];
    }
    return k;
}

// Дни от 1970-01-01 (алгоритм H. Hinnant, days_from_civil).
int64_t radex_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t radex_s2000(int year, int mon, int day, int hh, int mm, int ss)
{
    int64_t days = radex_days_from_civil(year, mon, day) - radex_days_from_civil(2000, 1, 1);
    return (uint32_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

uint64_t radex_ms2000(int year, int mon, int day, int hh, int mm, int ss, int ms)
{
    return (uint64_t)radex_s2000(year, mon, day, hh, mm, ss) * 1000ULL + (uint64_t)ms;
}
