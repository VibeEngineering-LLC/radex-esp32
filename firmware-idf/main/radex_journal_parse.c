/*#RADEX-281: разбор журнала Radex по NUS — чистая функция, тестируется на хосте (test/host/test_journal_parse.c)*/
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include "radex_journal_parse.h"

const uint8_t RADEX_J_CMD_SUMMARY[RADEX_J_CMD_LEN] = {0x47,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
const uint8_t RADEX_J_CMD_SUMMARY_NEXT[RADEX_J_CMD_LEN] = {0x81,0xff,0x00,0x00,0x00,0x00,0x00,0x00};
const uint8_t RADEX_J_CMD_RECORDS[RADEX_J_CMD_LEN] = {0x48,0x00,0x02,0x00,0x01,0x00,0x00,0x00};
const uint8_t RADEX_J_CMD_RECORDS_NEXT[RADEX_J_CMD_LEN] = {0x82,0xff,0x00,0x00,0x00,0x00,0x00,0x00};

bool radex_journal_cmd_allowed(const uint8_t *cmd, size_t len)
{
    if (cmd == NULL || len != RADEX_J_CMD_LEN) return false;
    static const uint8_t* cmds[] = {
        RADEX_J_CMD_SUMMARY,
        RADEX_J_CMD_SUMMARY_NEXT,
        RADEX_J_CMD_RECORDS,
        RADEX_J_CMD_RECORDS_NEXT
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); ++i) {
        if (memcmp(cmd, cmds[i], RADEX_J_CMD_LEN) == 0) return true;
    }
    return false;
}

bool radex_journal_records_cmd_ok(const uint8_t *cmd, size_t len, uint16_t last_record)
{
    if (cmd == NULL || len != RADEX_J_CMD_LEN) return false;
    if (cmd[0] != 0x48 || cmd[1] != 0x00 || cmd[6] != 0x00 || cmd[7] != 0x00) return false;
    uint16_t from = (uint16_t)(cmd[2] | (cmd[3] << 8));
    uint16_t to   = (uint16_t)(cmd[4] | (cmd[5] << 8));
    if (from >= last_record) return false;   /* индекс вне журнала этого сеанса (и нет сводки) */
    if (to > from) return false;
    return true;
}

bool radex_journal_cmd_allowed_ctx(const uint8_t *cmd, size_t len, uint16_t last_record)
{
    if (cmd == NULL || len != RADEX_J_CMD_LEN) return false;
    if (cmd[0] == 0x48) return radex_journal_records_cmd_ok(cmd, len, last_record);
    return radex_journal_cmd_allowed(cmd, len) && cmd[0] != 0x48;
}

const uint8_t RADEX_J_CCCD_ON[2]  = {0x01, 0x00};
const uint8_t RADEX_J_CCCD_OFF[2] = {0x00, 0x00};

bool radex_journal_records_range(uint16_t last_record, uint16_t *from, uint16_t *extra, bool *truncated)
{
    if (last_record == 0 || !from || !extra || !truncated) return false;
    *from = (uint16_t)(last_record - 1);
    *truncated = last_record > RADEX_J_MAX_REC;
    *extra = *truncated ? (uint16_t)(RADEX_J_MAX_REC - 1) : *from;
    return true;
}

uint16_t radex_journal_records_want(uint16_t extra)
{
    return (uint16_t)(extra + 1);   /* запрошенная запись + extra более старых */
}

void radex_journal_records_cmd_build(uint8_t out[RADEX_J_CMD_LEN], uint16_t from, uint16_t extra)
{
    const uint8_t b[RADEX_J_CMD_LEN] = { 0x48, 0x00, (uint8_t)from, (uint8_t)(from >> 8),
                                         (uint8_t)extra, (uint8_t)(extra >> 8), 0x00, 0x00 };
    memcpy(out, b, RADEX_J_CMD_LEN);
}

bool radex_journal_records_more(uint16_t got, uint16_t want, bool last_was_filler)
{
    return !last_was_filler && got < want;
}

bool radex_journal_cccd_allowed(const uint8_t *val, size_t len)
{
    if (val == NULL || len != 2) return false;
    return (val[0] == 0x01 && val[1] == 0x00) || (val[0] == 0x00 && val[1] == 0x00);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float rd_f32(const uint8_t *p)
{
    uint32_t u = rd_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static bool is_filler(const uint8_t *p, size_t len)
{
    if (len <= 2) return false;
    for (size_t i = 2; i < len; ++i) {
        if (p[i] != 0xff) return false;
    }
    return true;
}

int radex_journal_parse_summary(const uint8_t *p, size_t len, radex_journal_summary_t *out)
{
    if (p == NULL || out == NULL) return RADEX_J_BADARG;
    if (is_filler(p, len)) {
        memset(out, 0, sizeof(*out));
        if (len >= 2) out->seq = rd_u16(p);
        return RADEX_J_FILLER;
    }
    if (len < RADEX_J_SUMMARY_LEN) return RADEX_J_SHORT;
    out->seq = rd_u16(p);
    out->raw2 = rd_u16(p + 2);
    out->raw4 = rd_u16(p + 4);
    out->last_record = rd_u16(p + 6);
    out->time_raw = rd_u32(p + 8);
    out->avg = rd_f32(p + 12);
    out->sko = rd_f32(p + 16);
    return RADEX_J_OK;
}

int radex_journal_parse_record(const uint8_t *p, size_t len, radex_journal_record_t *out)
{
    if (p == NULL || out == NULL) return RADEX_J_BADARG;
    if (is_filler(p, len)) {
        memset(out, 0, sizeof(*out));
        if (len >= 2) out->seq = rd_u16(p);
        return RADEX_J_FILLER;
    }
    if (len < RADEX_J_RECORD_LEN) return RADEX_J_SHORT;
    out->seq = rd_u16(p);
    out->raw2 = rd_u16(p + 2);
    out->number = rd_u16(p + 4);
    out->time_raw = rd_u32(p + 6);
    out->unk_f10 = rd_f32(p + 10);
    out->oa = rd_f32(p + 14);
    out->temp_x10 = rd_u16(p + 18);
    out->raw20 = rd_u16(p + 20);
    out->humidity = p[22];
    out->flags = p[23];
    return RADEX_J_OK;
}

void radex_journal_raw_store(radex_journal_raw_t *dst, const uint8_t *p, size_t len)
{
    if (dst == NULL) return;
    memset(dst, 0, sizeof(*dst));
    if (p == NULL) return;
    size_t n = len < RADEX_J_RAW_MAX ? len : RADEX_J_RAW_MAX;
    memcpy(dst->data, p, n);
    dst->len = (uint8_t)n;
    dst->orig_len = len > 255 ? 255 : (uint8_t)len;
}

static bool app(char *buf, size_t len, size_t *o, const char *fmt, ...)
{
    if (buf == NULL || len == 0 || o == NULL || fmt == NULL) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *o, len - *o, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= len - *o) return false;
    *o += (size_t)n;
    return true;
}

static bool app_hex(char *buf, size_t len, size_t *o, const radex_journal_raw_t *r)
{
    if (buf == NULL || len == 0 || o == NULL || r == NULL) return false;
    if (!app(buf, len, o, "\"")) return false;
    for (size_t i = 0; i < r->len; ++i) {
        if (!app(buf, len, o, "%02x", r->data[i])) return false;
    }
    if (!app(buf, len, o, "\"")) return false;
    return true;
}

int radex_journal_json(const radex_journal_t *j, bool busy, bool pending, char *buf, size_t len)
{
    if (buf == NULL || len == 0 || j == NULL) return -1;
    size_t o = 0;
    if (!app(buf, len, &o, "{\"busy\":%s,\"pending\":%s,\"valid\":%s,\"ok\":%s",
            busy ? "true" : "false",
            pending ? "true" : "false",
            j->valid ? "true" : "false",
            j->ok ? "true" : "false")) return -1;

    char st[48] = {0};
    size_t st_len = 0;
    for (size_t i = 0; i < sizeof(j->status) && j->status[i] != '\0' && st_len < sizeof(st)-1; ++i) {
        unsigned char c = j->status[i];
        if (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\') {
            st[st_len++] = (char)c;
        }
    }
    st[st_len] = '\0';
    if (!app(buf, len, &o, ",\"status\":\"%s\",\"finished_s\":%lu,\"mtu\":%u", st,
             (unsigned long)j->finished_s, (unsigned)j->mtu)) return -1;
    if (!app(buf, len, &o, ",\"req_from\":%u,\"req_extra\":%u,\"truncated\":%s", (unsigned)j->req_from,
             (unsigned)j->req_extra, j->truncated ? "true" : "false")) return -1;

    if (!app(buf, len, &o, ",\"summary\":")) return -1;
    if (!j->have_summary) {
        if (!app(buf, len, &o, "null")) return -1;
    } else {
        const radex_journal_summary_t *s = &j->summary;
        if (!app(buf, len, &o, "{\"last_record\":%u,\"time_raw\":%lu,\"seq\":%u,\"raw2\":%u,\"raw4\":%u,\"avg\":%.2f,\"sko\":%.2f}",
                (unsigned)s->last_record, (unsigned long)s->time_raw, (unsigned)s->seq, (unsigned)s->raw2,
                (unsigned)s->raw4, (double)s->avg, (double)s->sko)) return -1;
    }

    uint8_t n_summary_pkt = j->n_summary_pkt > RADEX_J_MAX_PKT ? RADEX_J_MAX_PKT : j->n_summary_pkt;
    if (!app(buf, len, &o, ",\"summary_packets\":["))
        return -1;
    for (size_t i = 0; i < n_summary_pkt; ++i) {
        if (i > 0 && !app(buf, len, &o, ",")) return -1;
        if (!app_hex(buf, len, &o, &j->summary_pkt[i])) return -1;
    }
    if (!app(buf, len, &o, "]")) return -1;

    uint8_t n_records = j->n_records > RADEX_J_MAX_REC ? RADEX_J_MAX_REC : j->n_records;
    if (!app(buf, len, &o, ",\"records\":[")) return -1;
    for (size_t i = 0; i < n_records; ++i) {
        if (i > 0 && !app(buf, len, &o, ",")) return -1;
        const radex_journal_record_t *r = &j->records[i];
        if (!app(buf, len, &o, "{\"number\":%u,\"time_raw\":%lu,\"oa\":%.2f,\"t_x10\":%u,\"humidity\":%u,\"seq\":%u,\"raw2\":%u,\"unk_f10\":%.2f,\"raw20\":%u,\"flags\":%u}",
                (unsigned)r->number, (unsigned long)r->time_raw, (double)r->oa, (unsigned)r->temp_x10,
                (unsigned)r->humidity, (unsigned)r->seq, (unsigned)r->raw2, (double)r->unk_f10,
                (unsigned)r->raw20, (unsigned)r->flags)) return -1;
    }
    if (!app(buf, len, &o, "]")) return -1;

    uint8_t n_record_pkt = j->n_record_pkt > RADEX_J_MAX_REC + 1 ? RADEX_J_MAX_REC + 1 : j->n_record_pkt;
    if (!app(buf, len, &o, ",\"record_packets\":["))
        return -1;
    for (size_t i = 0; i < n_record_pkt; ++i) {
        if (i > 0 && !app(buf, len, &o, ",")) return -1;
        if (!app_hex(buf, len, &o, &j->record_pkt[i])) return -1;
    }
    if (!app(buf, len, &o, "]")) return -1;

    if (!app(buf, len, &o, "}")) return -1;
    return (int)o;
}
