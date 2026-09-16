// RADEX-281. Журнал читается по NUS по сценарию, повторяющему захват приложения дословно (reports/radex-ble-sniff-app-2026-09-16.md,
// «Захват 3»). Сериализация с круговым опросом: ble_radex.c зовёт tick из своего главного цикла и не начинает плановый круг, пока
// ble_radex_journal_busy(); сеанс начинается только при can_start (связь есть, круг опроса не идёт). Одна GATT-операция за раз: шаг
// ждёт подтверждения записи (и notify, если ожидается) до перехода к следующему. Таймаут на шаг — громкий лог и выход в обычный опрос.
// Запись в прибор — ТОЛЬКО через j_write_cmd/j_write_cccd, в которых handle зашит, а байты проверяются белым списком ДО отправки.

#include "ble_radex_journal.h"
#include "radex_journal_parse.h"
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

typedef enum { J_OP_CCCD_ON, J_OP_WRITE } j_op_t;
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
};
#define J_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

// Паузы и число запросов 81/82 (по два) скопированы с захвата; второй 81 вернул заглушку из 0xff.
// Параметры 0x48 НЕ поняты: 02 00 01 00 вернуло записи 3 и 2 из трёх. Запись — WRITE_TYPE_RSP
// (тип ATT-записи приложения в отчёте не указан): нужна квитанция, чтобы шаги не наложились.

typedef enum { J_IDLE, J_DELAY, J_WAIT } j_state_t;
static j_state_t        s_state = J_IDLE;       /* меняется только в tick (задача ble_radex) */
static volatile bool    s_pending;              /* запрос из Web */
static volatile bool    s_abort_disc;           /* разрыв, выставляется из GATTC-колбэка */
static int              s_step;
static TickType_t       s_t0;                   /* начало паузы шага */
static TickType_t       s_deadline;
static volatile bool    s_ack;
static volatile int     s_ack_status;
static volatile uint32_t s_notify_cnt;
static uint32_t         s_notify_base;
static esp_gatt_if_t    s_gif;
static uint16_t         s_conn;
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
    if (!radex_journal_cmd_allowed(cmd, len)) {
        ESP_LOGE(TAG, "ОТКАЗ: команда вне белого списка (len=%u, первый байт 0x%02X) — не отправлена", (unsigned)len, cmd ? cmd[0] : 0);
        return ESP_ERR_NOT_ALLOWED;
    }
    uint8_t tmp[RADEX_J_CMD_LEN];
    memcpy(tmp, cmd, RADEX_J_CMD_LEN);
    j_log_hex("TX", RADEX_NUS_H_RX, tmp, RADEX_J_CMD_LEN);
    return esp_ble_gattc_write_char(s_gif, s_conn, RADEX_NUS_H_RX, RADEX_J_CMD_LEN, tmp, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static esp_err_t j_write_cccd_on(void)
{
    static const uint8_t on[2] = {0x01, 0x00};
    if (!radex_journal_cccd_allowed(on, 2)) {
        ESP_LOGE(TAG, "ОТКАЗ: CCCD-запись вне белого списка — не отправлена");
        return ESP_ERR_NOT_ALLOWED;
    }
    uint8_t tmp[2];
    memcpy(tmp, on, 2);
    j_log_hex("TX", RADEX_NUS_H_CCCD, tmp, 2);
    return esp_ble_gattc_write_char_descr(s_gif, s_conn, RADEX_NUS_H_CCCD, 2, tmp, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static void j_finish(bool ok, const char *status)
{
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    s_work.valid = true;
    s_work.ok = ok;
    snprintf(s_work.status, sizeof(s_work.status), "%s", status);
    s_work.finished_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_result = s_work;
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
    s_active = false;
    s_state = J_IDLE;
    if (ok) {
        ESP_LOGI(TAG, "журнал прочитан: пакетов сводки %u, записей %u, пакетов записей %u", 
                 s_work.n_summary_pkt, s_work.n_records, s_work.n_record_pkt);
    } else {
        ESP_LOGE(TAG, "журнал НЕ прочитан: %s — возврат к обычному опросу", status);
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
    if (s_active) {
        s_abort_disc = true;
    }
}

static void j_start(esp_gatt_if_t gif, uint16_t conn, const uint8_t *bda)
{
    s_pending = false;
    s_abort_disc = false;
    s_gif = gif;
    s_conn = conn;
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
    memset(&s_work, 0, sizeof(s_work));
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
    esp_bd_addr_t a;
    memcpy(a, bda, sizeof(a));
    esp_err_t e = esp_ble_gattc_register_for_notify(gif, a, RADEX_NUS_H_TX);
    if (e != ESP_OK) {
        j_finish(false, "register_for_notify failed");   /* ASCII: статус уходит в JSON */
        return;
    }
    ESP_LOGI(TAG, "сеанс журнала: старт (%d шагов)", J_STEPS);
    s_step = 0;
    s_t0 = xTaskGetTickCount();
    s_active = true;
    s_state = J_DELAY;
}

void ble_radex_journal_tick(bool can_start, esp_gatt_if_t gattc_if, uint16_t conn_id, const uint8_t *bda)
{
    TickType_t now = xTaskGetTickCount();
    char st[48];
    if (s_state == J_IDLE) {
        if (s_pending && can_start && bda) {
            j_start(gattc_if, conn_id, bda);
        }
        return;
    }
    if (s_abort_disc) {
        snprintf(st, sizeof st, "disconnect: step %d", s_step);
        j_finish(false, st);
        return;
    }
    const j_step_t *sp = &s_steps[s_step];
    if (s_state == J_DELAY) {
        if ((now - s_t0) < pdMS_TO_TICKS(sp->delay_ms)) return;
        s_ack = false;
        s_ack_status = 0;
        s_notify_base = s_notify_cnt;
        esp_err_t e = (sp->op == J_OP_CCCD_ON) ? j_write_cccd_on() : j_write_cmd(sp->cmd, RADEX_J_CMD_LEN);
        if (e != ESP_OK) {
            snprintf(st, sizeof st, "send error 0x%x: step %d", (unsigned)e, s_step);
            j_finish(false, st);
            return;
        }
        s_deadline = now + pdMS_TO_TICKS(J_STEP_TIMEOUT_MS);
        s_state = J_WAIT;
        return;
    }
    /* J_WAIT */
    if (s_ack && s_ack_status != ESP_GATT_OK) {
        snprintf(st, sizeof st, "gatt status 0x%x: step %d", (unsigned)s_ack_status, s_step);
        j_finish(false, st);
        return;
    }
    bool got_notify = s_notify_cnt > s_notify_base;
    if (s_ack && (!sp->expect_notify || got_notify)) {
        s_step++;
        if (s_step >= J_STEPS) {
            j_finish(true, "ok");
            return;
        }
        s_t0 = now;
        s_state = J_DELAY;
        return;
    }
    if ((int32_t)(now - s_deadline) >= 0) {
        ESP_LOGE(TAG, "ТАЙМАУТ шага %d (%u мс): подтверждение записи=%d, notify=%d", s_step, (unsigned)J_STEP_TIMEOUT_MS, (int)s_ack, (int)got_notify);
        snprintf(st, sizeof st, "timeout: step %d", s_step);
        j_finish(false, st);
    }
}

void ble_radex_journal_on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.handle == RADEX_NUS_H_TX) {
                ESP_LOGI(TAG, "подписка на notify 0x%04X: status=%d", param->reg_for_notify.handle, (int)param->reg_for_notify.status);
            }
            break;
        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
            if (s_active && param->write.conn_id == s_conn && 
                (param->write.handle == RADEX_NUS_H_RX || param->write.handle == RADEX_NUS_H_CCCD)) {
                s_ack_status = param->write.status;
                s_ack = true;
                if (param->write.status != ESP_GATT_OK) {
                    ESP_LOGE(TAG, "запись в 0x%04X отвергнута прибором: status=0x%x", param->write.handle, (unsigned)param->write.status);
                }
            }
            break;
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.handle != RADEX_NUS_H_TX) {
                break;
            }
            if (!s_active || param->notify.conn_id != s_conn) {
                j_log_hex("RX вне сеанса", RADEX_NUS_H_TX, param->notify.value, param->notify.value_len);
                break;
            }
            j_log_hex("RX", RADEX_NUS_H_TX, param->notify.value, param->notify.value_len);
            if (s_mtx) {
                xSemaphoreTake(s_mtx, portMAX_DELAY);
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
            if (s_mtx) {
                xSemaphoreGive(s_mtx);
            }
            s_notify_cnt++;
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
    int n = radex_journal_json(&s_result, s_state != J_IDLE, s_pending, buf, len);
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
    return n;
}
