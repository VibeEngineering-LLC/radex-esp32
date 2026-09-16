// RADEX-281. Журнал читается по NUS по сценарию, повторяющему захват приложения дословно (reports/radex-ble-sniff-app-2026-09-16.md,
// «Захват 3»). Сериализация с круговым опросом: ble_radex.c зовёт tick из своего главного цикла и не начинает плановый круг, пока
// ble_radex_journal_busy(); сеанс начинается только при can_start (связь есть, круг опроса не идёт). Одна GATT-операция за раз: шаг
// ждёт подтверждения записи (и notify, если ожидается) до перехода к следующему. Таймаут на шаг — громкий лог и выход в обычный опрос.
// Запись в прибор — ТОЛЬКО через j_write_cmd/j_write_cccd, в которых handle зашит, а байты проверяются белым списком ДО отправки.

#include "ble_radex_journal.h"
#include "radex_journal_parse.h"
#include "radex_journal_track.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_gattc_api.h>
#include <esp_gatt_defs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define TAG "radex_j"

#define J_STEP_TIMEOUT_MS  3000
#define J_HEX_MAX          64      /* байт в строке hex-лога */

typedef enum { J_OP_CCCD_ON, J_OP_WRITE, J_OP_CCCD_OFF } j_op_t;
typedef enum { J_PH_SUMMARY, J_PH_RECORDS } j_phase_t;
typedef struct {
    j_op_t          op;
    const uint8_t  *cmd;            /* NULL для CCCD */
    uint16_t        delay_ms;       /* пауза перед шагом, как в захвате */
    bool            expect_notify;
    j_phase_t       phase;          /* куда относить пришедшие notify */
} j_step_t;
static const j_step_t s_steps[] = {
    { J_OP_CCCD_ON, NULL,                     0,   false, J_PH_SUMMARY },
    { J_OP_WRITE,   RADEX_J_CMD_SUMMARY,      0,   false, J_PH_SUMMARY },
    { J_OP_WRITE,   RADEX_J_CMD_SUMMARY_NEXT, 450, true,  J_PH_SUMMARY },
    { J_OP_WRITE,   RADEX_J_CMD_SUMMARY_NEXT, 0,   true,  J_PH_SUMMARY },
    { J_OP_WRITE,   RADEX_J_CMD_RECORDS,      300, false, J_PH_RECORDS },
    { J_OP_WRITE,   RADEX_J_CMD_RECORDS_NEXT, 100, true,  J_PH_RECORDS },
    { J_OP_WRITE,   RADEX_J_CMD_RECORDS_NEXT, 0,   true,  J_PH_RECORDS },
    /* аудит 281-B F7: выключить уведомления в конце сеанса (в захвате не было) */
    { J_OP_CCCD_OFF, NULL,                    0,   false, J_PH_RECORDS },
};
#define J_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

// Паузы и число запросов 81/82 (по два) скопированы с захвата; второй 81 вернул заглушку из 0xff.
// Параметры 0x48 НЕ поняты: 02 00 01 00 вернуло записи 3 и 2 из трёх. Запись — WRITE_TYPE_RSP
// (тип ATT-записи приложения в отчёте не указан): нужна квитанция, чтобы шаги не наложились.

/* MTU. Запись журнала — 24 байта; при ATT MTU 23 notify режется до 20 (теряются T и RH).
   Запрос MTU делаем НЕ в OPEN_EVT, а первым шагом сеанса: в OPEN_EVT он пересёкся бы с
   первым чтением опроса, которое ble_radex.c шлёт из UPDATE_CONN_PARAMS_EVT без ожидания.
   Здесь же круг опроса гарантированно стоит (can_start), так что операция одна. */
#define J_MTU_MIN  27
static volatile uint16_t s_mtu = 23;
static volatile bool     s_mtu_evt;
typedef enum { J_IDLE, J_MTU_WAIT, J_DELAY, J_WAIT } j_state_t;
static j_state_t        s_state = J_IDLE;       /* меняется только в tick (задача ble_radex) */
static volatile bool    s_pending;              /* запрос из Web */
static volatile int     s_step;                 /* пишет tick под s_mtx, читает колбэк */
static TickType_t       s_t0;                   /* начало паузы шага */
static TickType_t       s_deadline;
static radex_j_track_t  s_trk;                  /* под s_mtx: квитанции/notify по шагам */
static uint32_t         s_sess_gen;             /* поколение соединения, на котором идёт сеанс */
static esp_gatt_if_t    s_gif;
static volatile uint16_t s_conn;
static radex_journal_t  s_work;                 /* под s_mtx */
static radex_journal_t  s_result;               /* под s_mtx, отдаётся в Web */
static SemaphoreHandle_t s_mtx;
static volatile bool    s_active;               /* сеанс идёт: для колбэка */

static void j_log_hex(const char *dir, uint16_t handle, const uint8_t *p, size_t len)
{
    if (len > J_HEX_MAX) len = J_HEX_MAX;
    char hex[J_HEX_MAX * 3 + 1];
    for (size_t i = 0; i < len; ++i) {
        sprintf(hex + i*3, "%02x ", p[i]);
    }
    hex[len*3] = '\0';
    ESP_LOGI(TAG, "%s 0x%04X len=%u: %s", dir, handle, (unsigned)len, hex);
}

static esp_err_t j_write_cmd(const uint8_t *cmd, size_t len)
{
    /* аудит 281-A F5: сначала копия, проверяется и отправляется ОДНА и та же копия */
    uint8_t tmp[RADEX_J_CMD_LEN];
    if (cmd == NULL || len != RADEX_J_CMD_LEN) {
        ESP_LOGE(TAG, "ОТКАЗ: команда вне белого списка (len=%u) — не отправлена", (unsigned)len);
        return ESP_ERR_NOT_ALLOWED;
    }
    memcpy(tmp, cmd, RADEX_J_CMD_LEN);
    if (!radex_journal_cmd_allowed(tmp, RADEX_J_CMD_LEN)) {
        ESP_LOGE(TAG, "ОТКАЗ: команда вне белого списка (первый байт 0x%02X) — не отправлена", tmp[0]);
        return ESP_ERR_NOT_ALLOWED;
    }
    j_log_hex("TX", RADEX_NUS_H_RX, tmp, RADEX_J_CMD_LEN);
    return esp_ble_gattc_write_char(s_gif, s_conn, RADEX_NUS_H_RX, RADEX_J_CMD_LEN, tmp, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static esp_err_t j_write_cccd(const uint8_t *val)   /* val: RADEX_J_CCCD_ON или RADEX_J_CCCD_OFF */
{
    uint8_t tmp[2];
    if (val == NULL) return ESP_ERR_NOT_ALLOWED;
    memcpy(tmp, val, 2);
    if (!radex_journal_cccd_allowed(tmp, 2)) {
        ESP_LOGE(TAG, "ОТКАЗ: CCCD-запись вне белого списка — не отправлена");
        return ESP_ERR_NOT_ALLOWED;
    }
    j_log_hex("TX", RADEX_NUS_H_CCCD, tmp, 2);
    return esp_ble_gattc_write_char_descr(s_gif, s_conn, RADEX_NUS_H_CCCD, 2, tmp, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static void lk(void)  { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void ulk(void) { if (s_mtx) xSemaphoreGive(s_mtx); }

/* ok=true означает «все шаги пройдены»; итоговый статус решает radex_j_final_status:
   без записей, с коротким пакетом (MTU) или с событиями вне шага — не "ok" (281-B F2/F3/F5). */
static void j_finish(bool ok, const char *status)
{
    lk();
    s_work.valid = true;
    if (ok) {
        ok = radex_j_final_status(&s_work, &s_trk, s_work.status, sizeof(s_work.status));
    } else {
        snprintf(s_work.status, sizeof(s_work.status), "%s", status);
    }
    s_work.ok = ok;
    s_work.finished_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_active = false;
    s_result = s_work;
    unsigned stale = s_trk.stale_acks, unexp = s_trk.unexpected, outst = s_trk.outstanding;
    ulk();
    s_state = J_IDLE;
    if (ok) {
        ESP_LOGI(TAG, "журнал прочитан: пакетов сводки %u, записей %u, пакетов записей %u",
                 (unsigned)s_result.n_summary_pkt, (unsigned)s_result.n_records, (unsigned)s_result.n_record_pkt);
    } else {
        ESP_LOGE(TAG, "журнал НЕ прочитан: %s (поздних квитанций %u, событий вне шага %u, "
                      "неподтверждённых записей %u) — возврат к обычному опросу", s_result.status, stale, unexp, outst);
    }
}

void ble_radex_journal_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            ESP_LOGE(TAG, "не удалось создать мьютекс");
        }
    }
}

void ble_radex_journal_request(void)
{
    s_pending = true;
    ESP_LOGI(TAG, "запрошено чтение журнала");
}

bool ble_radex_journal_busy(void)
{
    return s_state != J_IDLE;
}

bool ble_radex_journal_pending(void)
{
    return s_pending;
}

void ble_radex_journal_on_disconnect(void)
{
    s_mtu = 23;   /* MTU живёт в пределах соединения */
    lk();
    radex_j_track_reset_all(&s_trk);   /* очередь Bluedroid сброшена вместе со связью */
    ulk();
    /* сам сеанс снимает tick: соединение не то (281-B F4) */
}

void ble_radex_journal_set_mtu(uint16_t mtu)   /* обмен MTU, начатый прибором (GATTS_MTU_EVT) */
{
    s_mtu = mtu;
}

static void j_start(esp_gatt_if_t gif, uint16_t conn, uint32_t gen, const uint8_t *bda)
{
    s_pending = false;
    s_gif = gif;
    s_conn = conn;
    s_sess_gen = gen;   /* 281-B F4: сеанс принадлежит этому соединению */
    lk();
    memset(&s_work, 0, sizeof(s_work));
    radex_j_track_new_session(&s_trk);
    s_step = 0;
    ulk();
    esp_bd_addr_t a;
    memcpy(a, bda, sizeof(a));
    esp_err_t e = esp_ble_gattc_register_for_notify(gif, a, RADEX_NUS_H_TX);
    if (e != ESP_OK) {
        j_finish(false, "register_for_notify failed");   /* ASCII: статус уходит в JSON */
        return;
    }
    ESP_LOGI(TAG, "сеанс журнала: старт (%d шагов), MTU=%u", J_STEPS, (unsigned)s_mtu);
    s_step = 0;
    s_t0 = xTaskGetTickCount();
    s_active = true;
    if (s_mtu >= J_MTU_MIN) { s_state = J_DELAY; return; }
    s_mtu_evt = false;
    e = esp_ble_gattc_send_mtu_req(gif, conn);   /* локальный MTU 247 задан в ble_radex.c */
    if (e != ESP_OK) { j_finish(false, "mtu req send failed"); return; }
    ESP_LOGW(TAG, "MTU=%u < %d — запрашиваю обмен MTU перед журналом", (unsigned)s_mtu, J_MTU_MIN);
    s_deadline = s_t0 + pdMS_TO_TICKS(J_STEP_TIMEOUT_MS);
    s_state = J_MTU_WAIT;
}

void ble_radex_journal_tick(bool can_start, bool connected, uint32_t conn_gen,
                            esp_gatt_if_t gattc_if, uint16_t conn_id, const uint8_t *bda)
{
    TickType_t now = xTaskGetTickCount();
    char st[48];
    if (s_state == J_IDLE) {
        lk();
        bool drained = radex_j_track_can_start(&s_trk);   /* поздние квитанции прежнего сеанса дошли */
        ulk();
        if (s_pending && can_start && drained && bda) {
            j_start(gattc_if, conn_id, conn_gen, bda);
        }
        return;
    }
    /* 281-B F4: разрыв в любой момент, включая окно между can_start и стартом, либо уже
       новое соединение — сеанс снимается, на чужом соединении он не продолжается. */
    if (!connected || conn_gen != s_sess_gen || conn_id != s_conn) {
        snprintf(st, sizeof st, "connection lost: step %d", (int)s_step);
        j_finish(false, st);
        return;
    }
    if (s_state == J_MTU_WAIT) {
        if (s_mtu_evt || (int32_t)(now - s_deadline) >= 0) {
            if (s_mtu < J_MTU_MIN) {   /* проверка перед сеансом: без MTU>=27 записи не читаем */
                ESP_LOGE(TAG, "MTU=%u < %d (обмен %s) — журнал НЕ читаю: запись 24 байта обрежется",
                         (unsigned)s_mtu, J_MTU_MIN, s_mtu_evt ? "завершён" : "не ответил");
                snprintf(st, sizeof st, "mtu %u < %d", (unsigned)s_mtu, J_MTU_MIN);
                j_finish(false, st);
                return;
            }
            s_t0 = now;
            s_state = J_DELAY;
        }
        return;
    }
    const j_step_t *sp = &s_steps[s_step];
    if (s_state == J_DELAY) {
        if ((now - s_t0) < pdMS_TO_TICKS(sp->delay_ms)) return;
        esp_err_t e = (sp->op == J_OP_CCCD_ON)  ? j_write_cccd(RADEX_J_CCCD_ON)
                    : (sp->op == J_OP_CCCD_OFF) ? j_write_cccd(RADEX_J_CCCD_OFF)
                    : j_write_cmd(sp->cmd, RADEX_J_CMD_LEN);
        if (e != ESP_OK) {   /* синхронный отказ: запись не ушла, квитанции не будет */
            snprintf(st, sizeof st, "send error 0x%x: step %d", (unsigned)e, (int)s_step);
            j_finish(false, st);
            return;
        }
        lk();
        radex_j_track_on_send(&s_trk, sp->op == J_OP_WRITE ? RADEX_NUS_H_RX : RADEX_NUS_H_CCCD,
                              sp->expect_notify);
        ulk();
        s_deadline = now + pdMS_TO_TICKS(J_STEP_TIMEOUT_MS);
        s_state = J_WAIT;
        return;
    }
    /* J_WAIT */
    lk();
    radex_j_track_t t = s_trk;
    bool done = radex_j_track_step_done(&s_trk);
    if (done) { radex_j_track_step_idle(&s_trk); s_step++; }   /* notify в паузе — уже «вне шага» */
    ulk();
    if (t.acked && t.ack_status != ESP_GATT_OK) {
        snprintf(st, sizeof st, "gatt status 0x%x: step %d", (unsigned)t.ack_status, (int)s_step);
        j_finish(false, st);
        return;
    }
    bool got_notify = t.notified;
    if (done) {
        if (s_step >= J_STEPS) {
            j_finish(true, "ok");
            return;
        }
        s_t0 = now;
        s_state = J_DELAY;
        return;
    }
    if ((int32_t)(now - s_deadline) >= 0) {
        ESP_LOGE(TAG, "ТАЙМАУТ шага %d (%u мс): подтверждение записи=%d, notify=%d; всё пришедшее позже будет отброшено",
                 (int)s_step, (unsigned)J_STEP_TIMEOUT_MS, (int)t.acked, (int)got_notify);
        snprintf(st, sizeof st, "timeout: step %d", (int)s_step);
        j_finish(false, st);
    }
}

void ble_radex_journal_on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTC_CFG_MTU_EVT:   /* и наш запрос, и обмен, начатый прибором */
            if (param->cfg_mtu.status == ESP_GATT_OK) s_mtu = param->cfg_mtu.mtu;
            ESP_LOGI(TAG, "MTU согласован: %u (status=%d)", (unsigned)param->cfg_mtu.mtu,
                     (int)param->cfg_mtu.status);
            s_mtu_evt = true;
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.handle == RADEX_NUS_H_TX) {
                ESP_LOGI(TAG, "подписка на notify 0x%04X: status=%d", param->reg_for_notify.handle, (int)param->reg_for_notify.status);
            }
            break;
        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
        {   /* 281-B F2 / 281-A F4: засчитывается только квитанция последней записи текущего шага */
            if (param->write.conn_id != s_conn) break;
            lk();
            radex_j_ev_t ev = radex_j_track_on_ack(&s_trk, param->write.handle, (int)param->write.status);
            ulk();
            if (ev == RJ_EV_FOREIGN) break;
            if (ev == RJ_EV_CURRENT && param->write.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "запись в 0x%04X отвергнута прибором: status=0x%x", param->write.handle, (unsigned)param->write.status);
            } else if (ev == RJ_EV_STALE) {
                ESP_LOGW(TAG, "поздняя квитанция 0x%04X (прежняя запись) отброшена", param->write.handle);
            } else if (ev == RJ_EV_UNEXPECTED) {
                ESP_LOGE(TAG, "квитанция 0x%04X вне шага отброшена — сеанс не будет ok", param->write.handle);
            }
            break;
        }
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.handle != RADEX_NUS_H_TX) {
                break;
            }
            if (!s_active || param->notify.conn_id != s_conn) {
                j_log_hex("RX вне сеанса", RADEX_NUS_H_TX, param->notify.value, param->notify.value_len);
                break;
            }
            j_log_hex("RX", RADEX_NUS_H_TX, param->notify.value, param->notify.value_len);
            lk();
            if (radex_j_track_on_notify(&s_trk) != RJ_EV_CURRENT) {   /* 281-B F3: не свой шаг */
                ulk();
                ESP_LOGE(TAG, "notify вне шага %d отброшен (длина %u) — сеанс не будет ok",
                         (int)s_step, (unsigned)param->notify.value_len);
                break;
            }
            j_phase_t ph = s_steps[s_step < J_STEPS ? s_step : J_STEPS - 1].phase;
            if (ph == J_PH_SUMMARY) {
                if (s_work.n_summary_pkt < RADEX_J_MAX_PKT) {
                    radex_journal_raw_store(&s_work.summary_pkt[s_work.n_summary_pkt++], param->notify.value, param->notify.value_len);
                }
                radex_journal_summary_t sm;
                int r = radex_journal_parse_summary(param->notify.value, param->notify.value_len, &sm);
                if (r == RADEX_J_OK && !s_work.have_summary) {
                    s_work.summary = sm;
                    s_work.have_summary = true;
                    ESP_LOGI(TAG, "сводка: последняя запись №%u, время(сыр.)=%lu", 
                             (unsigned)sm.last_record, (unsigned long)sm.time_raw);
                }
                ESP_LOGI(TAG, "пакет сводки: разбор=%d", r);
            } else {
                if (s_work.n_record_pkt < RADEX_J_MAX_PKT) {
                    radex_journal_raw_store(&s_work.record_pkt[s_work.n_record_pkt++], param->notify.value, param->notify.value_len);
                }
                radex_journal_record_t rec;
                int r = radex_journal_parse_record(param->notify.value, param->notify.value_len, &rec);
                if (r == RADEX_J_OK && s_work.n_records < RADEX_J_MAX_PKT) {
                    s_work.records[s_work.n_records++] = rec;
                    ESP_LOGI(TAG, "запись №%u: время(сыр.)=%lu ОА=%.2f Бк/м3 T×10=%u RH=%u%%",
                             (unsigned)rec.number, (unsigned long)rec.time_raw, (double)rec.oa,
                             (unsigned)rec.temp_x10, (unsigned)rec.humidity);
                }
                ESP_LOGI(TAG, "пакет записей: разбор=%d", r);
            }
            ulk();
            break;
        default:
            break;
    }
}

int ble_radex_journal_json(char *buf, size_t len)
{
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    s_result.mtu = s_mtu;   /* текущий MTU соединения, а не на момент сеанса */
    int n = radex_journal_json(&s_result, s_state != J_IDLE, s_pending, buf, len);
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
    return n;
}
