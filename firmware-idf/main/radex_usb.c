// #USB-1: USB-транспорт шлюза к Radex MR107ion. ESP32-S3 — USB-host (OTG-разъём), прибор — CDC-ACM
// ABBA:A204, 230400 8N1, DTR=0, RTS=1. Реализует ТЕ ЖЕ функции, что ble_radex.c и ble_radex_journal.c
// (ble_radex.h / ble_radex_journal.h): radex_data, история, веб-интерфейс, HTTP API и журнал выше шва
// не меняются. Собирается ВМЕСТО BLE-файлов при CONFIG_RADEX_TRANSPORT_USB (main/CMakeLists.txt).
// Доноры USB-host (§33, переиспользовано, не с нуля):
//   firmware/atomspectra-waterfall/main/usb_host_cdc.c — usb_host_lib_task, установка драйверов,
//     закрытие устройства вне колбэков драйвера (отложенный teardown);
//   skills/atomtex-esp32/components/bdkg_usb/bdkg_usb.c — data_cb -> StreamBuffer, транзакция
//     «запрос -> ответ» с дедлайном.
// Протокол: STATUS.md #USB-1, эталон ПК scripts/radex_usb/radex_ekosf.py. Прибору уходят ТОЛЬКО кадры из
// radex_ekosf.c, и каждый перед передачей проходит radex_req_allowed() (белый список из шести запросов).

#include "sdkconfig.h"
#include "ble_radex.h"
#include "ble_radex_journal.h"
#include "radex_journal_parse.h"
#include "radex_ekosf.h"
#include "journal_calib.h"
#include "net_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "radex_usb";

#define USB_TXN_TIMEOUT_MS   1500    // ответ на один RPC; в трассе ответы за единицы мс
#define USB_TX_TIMEOUT_MS    1000
#define USB_OPEN_TIMEOUT_MS  1000
#define USB_OPEN_RETRY_MS    2000
#define USB_POLL_RETRY_MS    10000   // повтор опроса после ошибки
#define USB_ERR_REOPEN       3       // столько ошибок опроса подряд — закрыть и открыть заново
#define USB_RX_BUF           2048    // длиннейший ответ (страница архива) — 506 байт
#define J_MAX_PAGES          4       // 64 записи = 3 страницы по 22; +1 запас

static ble_radex_cb_t        s_cb;
static cdc_acm_dev_hdl_t     s_dev;
static StreamBufferHandle_t  s_rx;
static volatile bool         s_dev_gone;      // из колбэка драйвера; закрывает задача
static volatile bool         s_connected;
static volatile uint32_t     s_read_ok, s_read_err, s_disconnects, s_open_fails;
static uint16_t              s_pnum;          // packetNum, растёт на каждый запрос (как у RadexDC)
static uint8_t               s_rxbuf[USB_RX_BUF];

static SemaphoreHandle_t     s_jmtx;
static volatile bool         s_j_pending, s_j_busy;
static radex_journal_t       s_jwork;         // только задача radex_usb
static radex_journal_t       s_jresult;       // под s_jmtx, отдаётся в Web

static uint32_t              s_dev_clock_s2000;   // часы прибора из последнего 0x0BC2
static bool                  s_have_dev_clock;
#if CONFIG_RADEX_USB_CLOCK_SYNC
static int64_t               s_last_sync_us;      // 0 — ещё не выставляли
#endif

// ── Колбэки драйвера: минимум работы, без закрытия устройства ────────────
static bool rx_cb(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (s_rx && len) xStreamBufferSend(s_rx, data, len, 0);
    return true;
}

static void ev_cb(const cdc_acm_host_dev_event_data_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: s_dev_gone = true; break;
    case CDC_ACM_HOST_ERROR: ESP_LOGW(TAG, "ошибка CDC: %d", e->data.error); s_dev_gone = true; break;
    default: break;
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t fl = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &fl);
        if (fl & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// ── Открытие / закрытие ───────────────────────────────────────────────────
static void dev_close(bool lost)
{
    if (s_dev) { cdc_acm_host_close(s_dev); s_dev = NULL; }
    if (s_connected && lost) s_disconnects++;
    s_connected = false;
    s_dev_gone = false;
}

static bool dev_open(void)
{
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = USB_OPEN_TIMEOUT_MS,
        .out_buffer_size = 64,
        .in_buffer_size = 512,
        .event_cb = ev_cb,
        .data_cb = rx_cb,
        .user_arg = NULL,
    };
    cdc_acm_dev_hdl_t h = NULL;
    esp_err_t e = cdc_acm_host_open(RADEX_USB_VID, RADEX_USB_PID, 0, &cfg, &h);
    if (e != ESP_OK) {
        // Прибор не вставлен — это не отказ открытия, а ожидание (ср. #RADEX-84: не смешивать счётчики).
        if (e != ESP_ERR_NOT_FOUND && e != ESP_ERR_TIMEOUT) {
            s_open_fails++;
            ESP_LOGW(TAG, "открыть прибор не удалось: %s", esp_err_to_name(e));
        }
        return false;
    }
    // DTR=0 — ПЕРВОЙ командой после открытия, до скорости: DTR=1 может сбросить прибор (#USB-1).
    // Сам cdc_acm_host_open линии не трогает. RTS=1 — рабочие параметры ПК-клиента и RadexDC.
    e = cdc_acm_host_set_control_line_state(h, false, true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "DTR=0/RTS=1 не выставлены (%s) — прибор не используем", esp_err_to_name(e));
        cdc_acm_host_close(h);
        s_open_fails++;
        return false;
    }
    const cdc_acm_line_coding_t lc = { .dwDTERate = RADEX_USB_BAUD, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8 };
    e = cdc_acm_host_line_coding_set(h, &lc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "230400 8N1 не выставлено (%s)", esp_err_to_name(e));
        cdc_acm_host_close(h);
        s_open_fails++;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));   // как ПК-клиент после выставления линий
    xStreamBufferReset(s_rx);
    s_dev_gone = false;
    s_dev = h;
    s_connected = true;
    ESP_LOGI(TAG, "прибор подключён по USB (%04X:%04X), 230400 8N1, DTR=0 RTS=1", RADEX_USB_VID, RADEX_USB_PID);
    return true;
}

// ── Одна транзакция запрос -> ответ ──────────────────────────────────────
// 0 — ответ принят (*res/*res_len указывают в s_rxbuf до следующей транзакции), <0 — ошибка.
static int txn(const uint8_t *f, size_t flen, uint16_t pnum, uint16_t rpc_id, const uint8_t **res, size_t *res_len)
{
    if (!s_dev) return -1;
    if (!radex_req_allowed(f, flen)) {
        ESP_LOGE(TAG, "ОТКАЗ: кадр вне белого списка (rpc 0x%04X) — не отправлен", (unsigned)rpc_id);
        return -2;
    }
    xStreamBufferReset(s_rx);
    esp_err_t e = cdc_acm_host_data_tx_blocking(s_dev, f, flen, USB_TX_TIMEOUT_MS);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "передача rpc 0x%04X: %s", (unsigned)rpc_id, esp_err_to_name(e));
        return -3;
    }
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_TXN_TIMEOUT_MS);
    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGW(TAG, "rpc 0x%04X: нет ответа за %d мс (принято %u байт)", (unsigned)rpc_id, USB_TXN_TIMEOUT_MS, (unsigned)got);
            return -4;
        }
        if (got >= sizeof(s_rxbuf)) return -6;
        got += xStreamBufferReceive(s_rx, s_rxbuf + got, sizeof(s_rxbuf) - got, deadline - now);
        if (got == 0) continue;
        size_t fl = 0;
        int pr = ekosf_parse_rsp(s_rxbuf, got, pnum, rpc_id, res, res_len, &fl);
        if (pr == EKOSF_RSP_OK) return 0;
        if (pr != EKOSF_RSP_NEED_MORE) {
            ESP_LOGW(TAG, "rpc 0x%04X: ответ отвергнут (%d)", (unsigned)rpc_id, pr);
            return -5;
        }
    }
}

// ── Текущие показания (0x0BC2) ───────────────────────────────────────────
static bool poll_current(radex_current_t *c)
{
    uint8_t f[EKOSF_TX_MAX];
    uint16_t pn = ++s_pnum;
    size_t n = radex_req_current(f, sizeof(f), pn);
    const uint8_t *res = NULL;
    size_t rl = 0;
    if (n == 0 || txn(f, n, pn, RADEX_RPC_CURRENT, &res, &rl) != 0 || !radex_parse_current(res, rl, c)) {
        s_read_err++;
        return false;
    }
    s_read_ok++;
    s_have_dev_clock = c->clock_ok;   // мусорные часы прибора не используем ни для калибровки, ни для сравнения
    if (c->clock_ok) s_dev_clock_s2000 = radex_s2000(2000 + c->yy, c->mo, c->dd, c->hh, c->mm, c->ss);
    return true;
}

// Точку в историю ставит main.c по RADEX_H_RADON_AVG, беря последние температуру и влажность, —
// поэтому среднее отдаётся ПОСЛЕДНИМ. Семантика 1:1 с BLE: radon_last (0x0049) — ОА последнего цикла
// (@12, сырая), radon_avg (0x0040) — средняя сессии прибора (@0), sko_avg (0x0043) — её СКО (@4).
// Скользящее среднее (@20, его показывает экран прибора) только в лог. Время измерения (0x0046) не опознано.
static void publish_current(const radex_current_t *c)
{
    ESP_LOGI(TAG, "ОА=%.2f скольз.=%.2f средн.=%.2f±%.2f Бк/м3 T=%.1f C RH=%.0f%% сессия %u/%u (часы прибора %02u:%02u:%02u %02u.%02u.%02u)",
             (double)c->last_oa, (double)c->cur, (double)c->avg, (double)c->sko, (double)c->temp_c, (double)c->rh,
             (unsigned)c->session, (unsigned)c->sessions,
             (unsigned)c->hh, (unsigned)c->mm, (unsigned)c->ss, (unsigned)c->dd, (unsigned)c->mo, (unsigned)c->yy);
    if (!s_cb) return;
    s_cb(RADEX_H_RADON_LAST, c->last_oa);
    s_cb(RADEX_H_TEMP, c->temp_c);
    s_cb(RADEX_H_HUMIDITY, c->rh);
    s_cb(RADEX_H_SKO_AVG, c->sko);
    s_cb(RADEX_H_RADON_AVG, c->avg);
}

// ── Часы прибора по NTP (0x0006) — единственная команда записи ──────────
#if CONFIG_RADEX_USB_CLOCK_SYNC
static void clock_sync_maybe(void)
{
    if (!s_dev || !s_have_dev_clock) return;
    if (!net_time_sntp_synced()) return;              // только после валидного NTP
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (!radex_time_valid(tv.tv_sec)) return;
    int64_t now_us = esp_timer_get_time();
    if (s_last_sync_us != 0 &&
        now_us - s_last_sync_us < (int64_t)CONFIG_RADEX_USB_CLOCK_SYNC_MIN_INTERVAL_H * 3600LL * 1000000LL) return;
    struct tm lt;
    localtime_r(&tv.tv_sec, &lt);   // прибор живёт в местном времени; пояс платы — из настроек (net_time)
    uint32_t board_s = radex_s2000(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    int64_t drift = (int64_t)board_s - (int64_t)s_dev_clock_s2000;
    if (drift < 0) drift = -drift;
    if (drift <= CONFIG_RADEX_USB_CLOCK_MAX_DRIFT_S) return;
    s_last_sync_us = now_us;                          // попытка считается и при неудаче: не долбить прибор
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &lt);
    uint64_t ms = radex_ms2000(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
                               (int)(tv.tv_usec / 1000));
    uint8_t f[EKOSF_TX_MAX];
    uint16_t pn = ++s_pnum;
    size_t n = radex_req_set_time(f, sizeof(f), pn, ms);
    const uint8_t *res = NULL;
    size_t rl = 0;
    if (n == 0 || txn(f, n, pn, RADEX_RPC_SET_TIME, &res, &rl) != 0) {
        ESP_LOGE(TAG, "часы прибора: установка не прошла (расхождение %lld с)", (long long)drift);
        return;
    }
    bool ok = rl >= 2 && res[0] == 0x01 && res[1] == 0x00;   // в трассе ответ 0100 = успех
    if (ok) ESP_LOGI(TAG, "часы прибора выставлены по NTP (расхождение было %lld с)", (long long)drift);
    else    ESP_LOGE(TAG, "часы прибора: прибор не подтвердил установку (расхождение %lld с)", (long long)drift);
}
#endif

// ── Журнал (архив прибора) ───────────────────────────────────────────────
static void j_finish(bool ok, const char *status)
{
    s_jwork.valid = true;
    s_jwork.ok = ok;
    snprintf(s_jwork.status, sizeof(s_jwork.status), "%s", status);
    s_jwork.finished_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_jwork.finished_unix = time(NULL);    // #RADEX-293: точка отсчёта калибровки — момент завершения
    s_jwork.mtu = 0;                        // по USB ATT MTU нет
    if (s_jmtx) xSemaphoreTake(s_jmtx, portMAX_DELAY);
    s_jresult = s_jwork;
    if (s_jmtx) xSemaphoreGive(s_jmtx);
    s_j_busy = false;
    if (ok) ESP_LOGI(TAG, "журнал прочитан: записей %u (последняя в приборе №%u)",
                     (unsigned)s_jwork.n_records, (unsigned)s_jwork.summary.last_record);
    else    ESP_LOGE(TAG, "журнал НЕ прочитан: %s", status);
}

// Последовательность RadexDC «Загрузить данные» (трассы captures/radex_usb_02_archive.pcap, _04_archive2.pcap):
// 0x0C04(текущая сессия) -> 0x0C0D (заголовок: сессии от новой к старой) -> 0x0C05(сквозной индекс) ->
// 0x0C0E страницами по 22 записи. Страницы идут СКВОЗЬ сессии по убыванию (трасса 04: после №1 сессии 1
// сразу №264 сессии 0), поэтому хватает одного 0x0C05 на последнюю запись: берём последние 64 записи
// архива, из скольких бы сессий они ни были. Каждая запись сверяется с ожидаемой парой (сессия, номер).
static bool j_begin(uint8_t session, radex_arch_hdr_t *h, const uint8_t **res, size_t *rl)
{
    uint8_t f[EKOSF_TX_MAX];
    uint16_t pn = ++s_pnum;
    size_t n = radex_req_arch_begin(f, sizeof(f), pn, session);
    if (n == 0 || txn(f, n, pn, RADEX_RPC_ARCH_BEGIN, res, rl) != 0) return false;
    pn = ++s_pnum;
    n = radex_req_arch_hdr(f, sizeof(f), pn);
    return n != 0 && txn(f, n, pn, RADEX_RPC_ARCH_HDR, res, rl) == 0 && radex_parse_arch_hdr(*res, *rl, h);
}

static void j_run(void)
{
    s_j_pending = false;
    s_j_busy = true;
    memset(&s_jwork, 0, sizeof(s_jwork));
    if (!s_dev) { j_finish(false, "usb: no device"); return; }
    uint8_t f[EKOSF_TX_MAX];
    const uint8_t *res = NULL;
    size_t rl = 0, n;
    uint16_t pn;
    char st[48];
    static radex_arch_hdr_t h;   // 300+ байт — не на стек задачи

    // Индекс текущей сессии — из 0x0BC2 (@76); RadexDC шлёт его в 0x0C04 ДО заголовка.
    radex_current_t c;
    if (!poll_current(&c)) { j_finish(false, "usb: step 0 (0x0BC2)"); return; }
    uint8_t session = (uint8_t)(c.session > 0xFF ? 0 : c.session);
    if (!j_begin(session, &h, &res, &rl)) { j_finish(false, "usb: step 1-2 (0x0C04/0x0C0D)"); return; }
    if (h.cur_session != session && h.cur_session <= 0xFF) {
        // смещение @76 — гипотеза по двум трассам; заголовок авторитетнее — один повтор с его значением
        ESP_LOGW(TAG, "журнал: сессия по 0x0BC2 = %u, по заголовку = %u — повторяю 0x0C04", (unsigned)session, (unsigned)h.cur_session);
        session = (uint8_t)h.cur_session;
        if (!j_begin(session, &h, &res, &rl) || h.cur_session != session) { j_finish(false, "usb: session mismatch"); return; }
    }
    radex_journal_raw_store(&s_jwork.summary_pkt[0], res, rl);
    s_jwork.n_summary_pkt = 1;
    uint32_t total = 0;
    for (uint8_t i = 0; i < h.n_sess; i++) total += h.sess[i].count;
    if (h.chain_ok && total != (uint32_t)h.last_gidx + 1) {
        snprintf(st, sizeof(st), "usb: header %lu != %u+1", (unsigned long)total, (unsigned)h.last_gidx);
        j_finish(false, st); return;
    }
    s_jwork.have_summary = true;
    s_jwork.summary.seq = 0;
    s_jwork.summary.raw2 = h.last_gidx;
    s_jwork.summary.raw4 = h.cur_session;
    s_jwork.summary.last_record = h.sess[0].count;
    s_jwork.summary.time_raw = h.sess[0].end_s2000;   // уточняется часами прибора ниже
    s_jwork.summary.avg = h.sess[0].avg;
    s_jwork.summary.sko = h.sess[0].sko;
    s_jwork.req_from = h.last_gidx;
    if (total == 0) { j_finish(false, "usb: archive empty"); return; }
    uint16_t want = total > RADEX_J_MAX_REC ? RADEX_J_MAX_REC : (uint16_t)total;
    s_jwork.truncated = total > RADEX_J_MAX_REC;

    pn = ++s_pnum; n = radex_req_arch_seek(f, sizeof(f), pn, h.last_gidx);
    if (n == 0 || txn(f, n, pn, RADEX_RPC_ARCH_SEEK, &res, &rl) != 0) { j_finish(false, "usb: step 3 (0x0C05)"); return; }

    // Ожидаемая пара (сессия, номер): начинаем с последней записи новейшей сессии, пустые сессии пропускаем.
    uint8_t si = 0;
    uint16_t exp_sess = h.cur_session;
    uint16_t exp_idx = h.sess[0].count;
    while (exp_idx == 0 && si + 1 < h.n_sess) { exp_sess = h.sess[si].prev_session; si++; exp_idx = h.sess[si].count; }
    for (int page = 0; page < J_MAX_PAGES && s_jwork.n_records < want; page++) {
        pn = ++s_pnum; n = radex_req_arch_page(f, sizeof(f), pn);
        if (n == 0 || txn(f, n, pn, RADEX_RPC_ARCH_PAGE, &res, &rl) != 0) {
            snprintf(st, sizeof(st), "usb: step 4 page %d (0x0C0E)", page);
            j_finish(false, st); return;
        }
        radex_arch_rec_t recs[RADEX_ARCH_PAGE_RECS];
        int k = radex_parse_arch_page(res, rl, recs, RADEX_ARCH_PAGE_RECS);
        if (k <= 0) { snprintf(st, sizeof(st), "usb: page %d empty", page); j_finish(false, st); return; }
        for (int i = 0; i < k && s_jwork.n_records < want; i++) {
            if (exp_idx == 0) { j_finish(false, "usb: more records than header"); return; }
            if (recs[i].raw0 != exp_sess || recs[i].idx != exp_idx) {
                s_jwork.seq_mismatch = true;
                snprintf(st, sizeof(st), "usb: seq %u/%u != %u/%u", (unsigned)recs[i].raw0, (unsigned)recs[i].idx,
                         (unsigned)exp_sess, (unsigned)exp_idx);
                j_finish(false, st); return;
            }
            radex_journal_record_t *r = &s_jwork.records[s_jwork.n_records];
            memset(r, 0, sizeof(*r));
            r->seq = recs[i].raw0;                            // по USB: индекс сессии прибора
            r->number = recs[i].idx;                          // номер внутри сессии
            r->time_raw = recs[i].time_s2000;
            r->oa = recs[i].oa;
            r->unk_f10 = recs[i].avg;                         // по USB опознано: скользящее среднее
            r->temp_x10 = (uint16_t)(recs[i].t_x10 & 0xFFFF);
            r->raw20 = (uint16_t)(recs[i].t_x10 >> 16);
            r->humidity = recs[i].rh;
            r->flags = recs[i].flags;
            radex_journal_raw_store(&s_jwork.record_pkt[s_jwork.n_record_pkt++],
                                    res + 4 + (size_t)i * RADEX_ARCH_REC_LEN, RADEX_ARCH_REC_LEN);
            s_jwork.n_records++;
            exp_idx--;
            while (exp_idx == 0 && si + 1 < h.n_sess) { exp_sess = h.sess[si].prev_session; si++; exp_idx = h.sess[si].count; }
        }
    }
    if (s_jwork.n_records < want) {
        snprintf(st, sizeof(st), "usb: got %u of %u records", (unsigned)s_jwork.n_records, (unsigned)want);
        j_finish(false, st); return;
    }

    // #RADEX-293 калибровка: offset = часы шлюза - summary.time_raw (journal_calib.h). По USB время
    // записей — в эпохе часов прибора, поэтому точка отсчёта — ЧАСЫ ПРИБОРА сейчас (0x0BC2), а не
    // время последней записи (так по BLE): смещение точное, а не с ошибкой до цикла 600 с.
    if (poll_current(&c) && s_have_dev_clock) s_jwork.summary.time_raw = s_dev_clock_s2000;
    else ESP_LOGW(TAG, "журнал: часы прибора не прочитаны — отсчёт от последней записи, как по BLE");
    j_finish(true, "ok");
}

// ── Главная задача ───────────────────────────────────────────────────────
static void radex_usb_task(void *arg)
{
    (void)arg;
    int64_t next_poll_us = 0;
    int consec_err = 0;
    while (1) {
        if (s_dev && s_dev_gone) {
            ESP_LOGW(TAG, "прибор отключился от USB");
            dev_close(true);
        }
        if (!s_dev) {
            if (s_j_pending) j_run();             // сразу ответить «нет прибора», не держать очередь
            if (!dev_open()) { vTaskDelay(pdMS_TO_TICKS(USB_OPEN_RETRY_MS)); continue; }
            next_poll_us = 0;
            consec_err = 0;
        }
        if (s_j_pending && !s_j_busy) { j_run(); continue; }
        int64_t now = esp_timer_get_time();
        if (now >= next_poll_us) {
            radex_current_t c;
            if (poll_current(&c)) {
                consec_err = 0;
                publish_current(&c);
#if CONFIG_RADEX_USB_CLOCK_SYNC
                clock_sync_maybe();
#endif
                next_poll_us = now + (int64_t)CONFIG_RADEX_USB_POLL_PERIOD_S * 1000000LL;
            } else {
                consec_err++;
                next_poll_us = now + (int64_t)USB_POLL_RETRY_MS * 1000LL;
                if (consec_err >= USB_ERR_REOPEN) {
                    ESP_LOGW(TAG, "%d ошибок опроса подряд — переоткрываю прибор", consec_err);
                    dev_close(true);
                    consec_err = 0;
                    vTaskDelay(pdMS_TO_TICKS(USB_OPEN_RETRY_MS));
                    continue;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── Запуск ────────────────────────────────────────────────────────────────
// В отличие от BLE-версии управление ВОЗВРАЩАЕТСЯ: вся работа — в своей задаче; main.c после вызова
// уходит в свой вечный цикл.
void ble_radex_start(ble_radex_cb_t cb)
{
    s_cb = cb;
    ble_radex_journal_init();
    s_rx = xStreamBufferCreate(USB_RX_BUF, 1);
    if (!s_rx) { ESP_LOGE(TAG, "нет памяти под буфер приёма"); return; }
    const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t e = usb_host_install(&hc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(e)); return; }
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 7, NULL, 0);
    const cdc_acm_host_driver_config_t dc = {
        .driver_task_stack_size = 4096, .driver_task_priority = 8, .xCoreID = 0, .new_dev_cb = NULL,
    };
    e = cdc_acm_host_install(&dc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(e)); return; }
    xTaskCreatePinnedToCore(radex_usb_task, "radex_usb", 6144, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "USB-транспорт запущен, жду прибор %04X:%04X", RADEX_USB_VID, RADEX_USB_PID);
}

// ── Выбор прибора: по USB выбирать нечего (прибор один, на кабеле) ───────
bool ble_radex_has_target(void) { return true; }
bool ble_radex_scanning(void)   { return false; }
int  ble_radex_found_json(char *buf, size_t len)
{
    return snprintf(buf, len, "{\"scanning\":false,\"has_target\":true,\"found\":[]}");
}
bool ble_radex_set_target(const char *mac_str)
{
    (void)mac_str;
    ESP_LOGW(TAG, "USB-транспорт: выбор прибора по MAC не применяется");
    return false;
}
bool ble_radex_target_mac(char *buf, size_t len)
{
    if (buf && len) buf[0] = '\0';
    return false;
}
void ble_radex_clear_target(void)
{
    ESP_LOGW(TAG, "USB-транспорт: привязки по MAC нет — сбрасывать нечего");
}

bool     ble_radex_connected(void)   { return s_connected; }
uint32_t ble_radex_reads_ok(void)    { return s_read_ok; }
uint32_t ble_radex_read_errors(void) { return s_read_err; }
uint32_t ble_radex_disconnects(void) { return s_disconnects; }
uint32_t ble_radex_open_fails(void)  { return s_open_fails; }

// ── Журнал: API для web_server.c ─────────────────────────────────────────
void ble_radex_journal_init(void)
{
    if (!s_jmtx) s_jmtx = xSemaphoreCreateMutex();
}
void ble_radex_journal_request(void)
{
    s_j_pending = true;
    ESP_LOGI(TAG, "запрошено чтение журнала");
}
bool ble_radex_journal_busy(void)    { return s_j_busy; }
bool ble_radex_journal_pending(void) { return s_j_pending; }
int  ble_radex_journal_json(char *buf, size_t len)
{
    if (s_jmtx) xSemaphoreTake(s_jmtx, portMAX_DELAY);
    int n = radex_journal_json(&s_jresult, s_j_busy, s_j_pending, buf, len);
    if (s_jmtx) xSemaphoreGive(s_jmtx);
    return n;
}
bool ble_radex_journal_get_result(radex_journal_t *out)
{
    if (!out) return false;
    if (s_jmtx) xSemaphoreTake(s_jmtx, portMAX_DELAY);
    *out = s_jresult;
    if (s_jmtx) xSemaphoreGive(s_jmtx);
    return true;
}
