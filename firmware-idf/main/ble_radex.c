// ==========================================================================
//  ble_radex.c - BLE-клиент к радон-детектору Radex MR107ion.
//
//  ПРОИСХОЖДЕНИЕ: перенесён из диагностического полигона
//  firmware/radex-idf/main/radex_main.c, где был доказан замером:
//  705 успешных чтений / 0 разрывов за 900 с (2026-08-27, лог
//  logs/hci/s3_autodisc_off_v2_20260827.log). Логика чтения не менялась -
//  изменён только способ выдачи значений: вместо печати в лог вызывается
//  колбэк, заданный ble_radex_start().
//
//  КЛЮЧЕВОЕ (#RADEX-4 H44). Bluedroid по умолчанию сам запускает service
//  discovery на каждом соединении (auto_disc=TRUE, bta_gattc_act.c). У
//  MR107ion он никогда не завершается, и наши ATT-чтения встают в очередь
//  позади него - прибор трое суток выглядел "не отвечающим". Отключается
//  недокументированным BTA_GATTC_AutoDiscoverEnable(0), причём ТОЛЬКО из
//  обработчика ESP_GATTC_REG_EVT: вызов из app_main затирается поздним
//  сбросом флага внутри bta_gattc_enable().
// ==========================================================================
#include "ble_radex.h"
#include <stdio.h>
#include <string.h>
#include <esp_bt.h>
#include <esp_idf_version.h>   /* #RADEX-86: cancel_open появился только в IDF 6.x */
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_common_api.h>
#include <esp_gatts_api.h>
#include <esp_log.h>
#include <esp_system.h>   /* #RADEX-145: esp_reset_reason/esp_restart */
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern uint8_t BTA_GATTC_AutoDiscoverEnable(uint8_t enable);

#define TAG "radex"

#define RADEX_PRE_READ_MS      4000
#define RADEX_MTU_FALLBACK_MS  5000
#define RADEX_LOCAL_PRIVACY    1

// Адрес прибора НЕ зашивается в прошивку: у каждого пользователя он свой, а
// публиковать реальный MAC нельзя. Порядок: NVS -> если пусто, ищем в эфире
// по имени (MR107*/Radex*) и предъявляем найденное в Web UI.
#define RADEX_NVS_NS    "radex"
#define RADEX_NVS_MAC   "mac"
#define RADEX_NVS_ATYPE "atype"

static esp_bd_addr_t s_target_addr;
static uint8_t       s_target_atype = BLE_ADDR_TYPE_RANDOM;
static bool          s_have_target  = false;


#define H_RADON_LAST  0x0049
#define H_RADON_AVG   0x0040
#define H_TEMP_X10    0x0058
#define H_HUMIDITY    0x005E
/* #RADEX-18: прибор сам считает СКО своего среднего и время измерения.
   Наше среднее строится по выборке раз в 10 минут, приборное — по всем его
   замерам, поэтому приборное точнее, а СКО даёт статистическую неопределённость
   в процентах (пункт 10.1 ТЗ). Время измерения нужно, чтобы понять, за какой
   период это среднее посчитано: от включения прибора, а не от начала теста. */
#define H_SKO_AVG     0x0043
#define H_T_IZM       0x0046

/* #RADEX-18: за сессию прибор успевает отдать РОВНО ЧЕТЫРЕ значения — замер
   28.08: чтения идут по 2-5 с, на пятом прибор рвёт связь (status=133/259,
   reason=8 через ~20 с после подключения). Поэтому круг всегда из четырёх:
   два первых — радон (он нужен каждый раз и идёт в оценку), два оставшихся
   берутся по очереди из остальных величин. Температура, влажность, СКО и
   время измерения меняются медленно, обновления раз в 20-40 минут им хватает. */
#define POLL_FIXED  2       /* radon_last + radon_avg — всегда */
#define POLL_ROT    2       /* сколько ротируемых берём за круг */

static const uint16_t s_fixed[POLL_FIXED] = { H_RADON_LAST, H_RADON_AVG };
static const uint16_t s_rot[] = { H_TEMP_X10, H_HUMIDITY, H_SKO_AVG, H_T_IZM };
static const int s_rot_n = 4;
static int s_rot_pos = 0;               /* сдвигается на POLL_ROT каждый круг */

static uint16_t s_handles[POLL_FIXED + POLL_ROT];
static const int s_handle_count = POLL_FIXED + POLL_ROT;

/* Собрать круг: фиксированные + следующая пара ротируемых. */
static void poll_build(void)
{
    for (int i = 0; i < POLL_FIXED; i++) s_handles[i] = s_fixed[i];
    for (int i = 0; i < POLL_ROT; i++)
        s_handles[POLL_FIXED + i] = s_rot[(s_rot_pos + i) % s_rot_n];
    s_rot_pos = (s_rot_pos + POLL_ROT) % s_rot_n;
}

esp_gatt_if_t s_gattc_if;
uint16_t s_conn_id;
uint8_t s_handle_cursor;
SemaphoreHandle_t s_reconnect_sem;
static volatile int s_connected = 0;
volatile TickType_t s_open_tick;
volatile int s_mtu_state = 0;
static volatile uint32_t s_read_ok, s_read_err, s_disconnects;

/* #RADEX-84. Разрыв установленного соединения и неудачная попытка его открыть —
   разные события, и мерить их одним счётчиком нельзя. Счётчик «разрывов связи» за
   38 минут показал 50 694 — оператор резонно прочитал это как связь, рвущуюся
   двадцать раз в секунду. Связи не было ни секунды: это 50 694 неудачных попытки
   её открыть.
   Уточнение после стерильного разбора: приходящий в этом случае reason=0x100 —
   это ESP_GATT_CONN_CONN_CANCEL, то есть решение ХОСТА снять свою попытку. Он не
   является ответом контроллера и ничего не говорит о том, есть ли прибор в эфире;
   прежняя формулировка здесь приписывала коду смысл, которого у него нет. */
static volatile uint32_t s_open_fails;

/* #RADEX-145: после реального разрыва (rsn=0x8) контроллер иногда вешает
   КАЖДУЮ следующую попытку `open` на `Cmd Disallowed` (opcode 0x2043)
   навсегда — залипание внутри контроллера, `cancel_open` (RADEX-86) его
   не видит («No such connection need to be cancelled» = хост свободен).
   Единственное штатное лекарство без физического power-cycle — переинит
   BT через `esp_restart()`: подтверждено на живой плате (см. журнал
   #RADEX-145). Порог — счётчик подряд неудачных `open` с последнего
   успеха, см. ниже. */
#define RADEX_AUTO_RESTART_AFTER_FAILS  8
static volatile uint32_t s_consec_open_fails;

/* Пауза перед повторной попыткой, растущая вдвое до потолка. Без неё задача
   переподключения крутится со скоростью ответа контроллера (~25 попыток в
   секунду): эфир засоряется и лог тонет — ради этого пауза и введена.
   ВАЖНО, что пауза НЕ лечит: «Cmd Disallowed» приходит одинаково и через 1 с, и
   через 30 с (лог diag-trace-20260829). Прежнее объяснение «предыдущая попытка
   не завершена, когда шлётся следующая» этим же логом опровергнуто. */
#define RADEX_RECONNECT_MIN_MS   1000
#define RADEX_RECONNECT_MAX_MS  30000
static volatile uint32_t s_reconnect_delay_ms = RADEX_RECONNECT_MIN_MS;

// #RADEX-7 (2026-08-28, оператор: «не надо так часто парсить прибор»).
// Опрос шёл по кругу без пауз: каждый ответ немедленно тянул следующее
// чтение — 196 чтений за 5 минут. Радону такая частота не нужна: прибор
// меряет раз в 60 с, а значение меняется часами (замер 28.08: 113 чтений
// подряд дали 6 разных значений). Держим связь, но между кругами молчим.
#define RADEX_POLL_PERIOD_MS  600000   // 10 минут между кругами опроса
static volatile TickType_t s_next_poll_tick;   // когда начать следующий круг
static volatile int s_poll_active;             // круг идёт прямо сейчас

// #RADEX-153b (31.08): строка «статус: …» печаталась безусловно раз в 10 с и
// вытесняла из кольцевого лога (24 КБ, ~250 строк) реальные события. Замер до
// правки на живой плате: в выдаче /api/log 76 строк из 82 — одна и та же
// НЕизменившаяся строка, видимое окно журнала всего 12,5 мин. Разрывы, отказы
// чтения, неудачные open и перезапуски #RADEX-145 уезжали из кольца за минуты,
// и к моменту, когда человек открывает лог, их там уже не было.
// Печатаем по ИЗМЕНЕНИЮ четвёрки (чтения/ошибки/разрывы/связь) плюс редкий
// heartbeat — чтобы молчание не путали с зависшей платой. Сами счётчики и
// watchdog #RADEX-145 не затронуты: меняется только условие печати.
#define RADEX_STATUS_CHECK_TICKS   100   // проверять состояние раз в 10 с
#define RADEX_STATUS_HEARTBEAT_N    60   // 60 × 10 с = heartbeat раз в 10 мин

static ble_radex_cb_t s_cb = NULL;

// Значение получено. Печать оставлена (её видно в кольцевом логе Web UI),
// но основная выдача - колбэк потребителю.
static void radex_on_measurement(uint16_t handle, float value)
{
    switch (handle) {
        case 0x0049: ESP_LOGI(TAG, "радон (последний) = %.2f Бк/м3", value); break;
        case 0x0040: ESP_LOGI(TAG, "радон (среднее прибора) = %.2f Бк/м3", value); break;
        case 0x0058: ESP_LOGI(TAG, "температура = %.1f C", value); break;
        case 0x005E: ESP_LOGI(TAG, "влажность = %.0f %%", value); break;
        default:     ESP_LOGI(TAG, "handle 0x%04X = %.3f", handle, value); break;
    }
    if (s_cb) s_cb(handle, value);
}

static int radex_decode(uint16_t handle, const uint8_t *data, uint16_t len, float *out)
{
    if (len < 1) return 0;

    switch (handle) {
        case H_RADON_LAST:
        case H_RADON_AVG:
            if (len < 4) return 0;
            memcpy(out, data, 4);
            return 1;
        case H_TEMP_X10:
            if (len < 2) return 0;
            int16_t temp;
            memcpy(&temp, data, 2);
            *out = (float)temp / 10.0f;
            return 1;
        case H_HUMIDITY:
            *out = (float)data[0];
            return 1;
        case H_SKO_AVG:
            if (len < 4) return 0;
            memcpy(out, data, 4);          /* float32 LE, Бк/м3 */
            return 1;
        case H_T_IZM: {
            if (len < 4) return 0;
            uint32_t secs;
            memcpy(&secs, data, 4);        /* uint32 LE, секунды */
            *out = (float)secs;
            return 1;
        }
        default:
            return 0;
    }
}

static void issue_read(void)
{
    esp_err_t ret = esp_ble_gattc_read_char(s_gattc_if, s_conn_id, s_handles[s_handle_cursor], ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "чтение handle 0x%04X отказано, status=%d", s_handles[s_handle_cursor], ret);
    }
}

static void start_polling(const char *why)
{
    s_mtu_state = 1;
    s_handle_cursor = 0;
    s_poll_active = 1;
    poll_build();      /* каждый круг — своя пара ротируемых величин */
    ESP_LOGI(TAG, "старт опроса (%s)", why);
    issue_read();
}


// ── Поиск прибора в эфире ─────────────────────────────────────────────────
// Прибор рекламируется именем вида "MR107ion NNNN". Ищем по префиксу имени:
// сервис FE651700 в рекламе не всегда присутствует, а имя есть всегда.
#define SCAN_MAX 8
typedef struct {
    esp_bd_addr_t addr;
    uint8_t       atype;
    int8_t        rssi;
    char          name[24];
} radex_found_t;

static radex_found_t s_found[SCAN_MAX];
static uint8_t       s_found_n = 0;
static bool          s_scanning = false;

static bool name_looks_like_radex(const char *n, int len)
{
    if (len <= 0) return false;
    return (len >= 6 && strncasecmp(n, "MR107", 5) == 0) ||
           (len >= 5 && strncasecmp(n, "Radex", 5) == 0);
}

static void target_save(const esp_bd_addr_t addr, uint8_t atype)
{
    nvs_handle_t h;
    if (nvs_open(RADEX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, RADEX_NVS_MAC, addr, sizeof(esp_bd_addr_t));
    nvs_set_u8(h, RADEX_NVS_ATYPE, atype);
    nvs_commit(h);
    nvs_close(h);
}

static bool target_load(void)
{
    nvs_handle_t h;
    if (nvs_open(RADEX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(esp_bd_addr_t);
    bool ok = (nvs_get_blob(h, RADEX_NVS_MAC, s_target_addr, &len) == ESP_OK &&
               len == sizeof(esp_bd_addr_t));
    if (ok) {
        uint8_t a = BLE_ADDR_TYPE_RANDOM;
        nvs_get_u8(h, RADEX_NVS_ATYPE, &a);
        s_target_atype = a;
    }
    nvs_close(h);
    return ok;
}

static void scan_start(void)
{
    static esp_ble_scan_params_t sp = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,   // нужен SCAN_RSP: имя приходит в нём
        .own_addr_type      = BLE_ADDR_TYPE_RANDOM,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    s_found_n = 0;
    s_scanning = true;
    esp_ble_gap_set_scan_params(&sp);   // старт скана — в SCAN_PARAM_SET_COMPLETE
    ESP_LOGI(TAG, "прибор не задан — ищу в эфире");
}

// ── Публичный API поиска (для Web UI) ─────────────────────────────────────
bool ble_radex_has_target(void) { return s_have_target; }

// #RADEX-190: сброс привязки. Чистим NVS и поднимаем скан заново; текущее
// соединение рвём, иначе плата продолжит читать прежний (чужой) прибор.
void ble_radex_clear_target(void)
{
    nvs_handle_t h;
    if (nvs_open(RADEX_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, RADEX_NVS_MAC);
        nvs_erase_key(h, RADEX_NVS_ATYPE);
        nvs_commit(h);
        nvs_close(h);
    }
    s_have_target = false;
    memset(s_target_addr, 0, sizeof(s_target_addr));
    ESP_LOGW(TAG, "привязка к прибору сброшена — ищу в эфире заново");
    if (s_connected) esp_ble_gattc_close(s_gattc_if, s_conn_id);
    scan_start();
}

// #RADEX-188: тот же адрес, но строкой — Web UI показывает его на вкладке
// «Системные», чтобы было видно, к какому именно прибору привязана плата
// (в эфире может быть чужой Radex, и раньше это ничем не выдавалось).
bool ble_radex_target_mac(char *buf, size_t len)
{
    if (!buf || len < 18) return false;
    if (!s_have_target) { buf[0] = '\0'; return false; }
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             s_target_addr[0], s_target_addr[1], s_target_addr[2],
             s_target_addr[3], s_target_addr[4], s_target_addr[5]);
    return true;
}
bool ble_radex_scanning(void)   { return s_scanning; }

int ble_radex_found_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "{\"scanning\":%s,\"has_target\":%s,\"found\":[",
                     s_scanning ? "true" : "false", s_have_target ? "true" : "false");
    for (uint8_t i = 0; i < s_found_n && n > 0 && n < (int)len; i++) {
        n += snprintf(buf + n, len - n,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"name\":\"%s\",\"rssi\":%d}",
            i ? "," : "",
            s_found[i].addr[0], s_found[i].addr[1], s_found[i].addr[2],
            s_found[i].addr[3], s_found[i].addr[4], s_found[i].addr[5],
            s_found[i].name, s_found[i].rssi);
    }
    if (n > 0 && n < (int)len) n += snprintf(buf + n, len - n, "]}");
    return n;
}

// Принимает MAC вида AA:BB:CC:DD:EE:FF. Сохраняет и перезагружает плату:
// стек уже поднят под другую цель, чистый рестарт надёжнее донастройки.
bool ble_radex_set_target(const char *mac_str)
{
    unsigned v[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6)
        return false;
    esp_bd_addr_t a;
    uint8_t atype = BLE_ADDR_TYPE_RANDOM;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return false;
        a[i] = (uint8_t) v[i];
    }
    // Если адрес есть среди найденных — берём его тип адреса, он достовернее.
    for (uint8_t i = 0; i < s_found_n; i++)
        if (memcmp(s_found[i].addr, a, sizeof(a)) == 0) atype = s_found[i].atype;
    target_save(a, atype);
    /* #RADEX-89: скан останавливаем ЯВНО. Дальше идёт перезагрузка, и радио
       освободилось бы и так, но между выбором прибора и рестартом остаётся
       окно, где скан продолжает занимать радио уже без всякой цели: искать
       больше нечего. Флаг снимаем вместе с командой — иначе состояние в
       /api/scan продолжало бы утверждать, что поиск идёт. */
    if (s_scanning) {
        esp_ble_gap_stop_scanning();
        s_scanning = false;
    }
    ESP_LOGW(TAG, "прибор задан: %02X:%02X:%02X:%02X:%02X:%02X — перезагрузка",
             a[0],a[1],a[2],a[3],a[4],a[5]);
    return true;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            /* #RADEX-89: статус применения параметров проверяем ЯВНО. Раньше он
               молча игнорировался, и отказ («No random address yet» на чистом
               старте, когда случайный адрес контроллером ещё не сгенерирован)
               выглядел как успех: скан всё равно поднимался, но на умолчаниях
               контроллера, а не на заданных интервале/окне/отсеве повторов.
               Сам тип адреса (own_addr_type) НЕ трогаем — он входит в
               доказанную серию соединения (#RADEX-145), менять его без замера
               на свободной плате нельзя. */
            if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "параметры скана НЕ применены (status=%d) — контроллер работает на умолчаниях",
                         (int) param->scan_param_cmpl.status);
            }
            if (s_scanning) esp_ble_gap_start_scanning(0);   // 0 = до остановки
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            struct ble_scan_result_evt_param *r = &param->scan_rst;
            if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
            uint8_t nlen = 0;
            uint8_t *nm = esp_ble_resolve_adv_data(r->ble_adv,
                              ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
            if (!nm || !name_looks_like_radex((const char *)nm, nlen)) break;
            for (uint8_t i = 0; i < s_found_n; i++)
                if (memcmp(s_found[i].addr, r->bda, sizeof(esp_bd_addr_t)) == 0) {
                    s_found[i].rssi = r->rssi;
                    goto scan_done;
                }
            if (s_found_n < SCAN_MAX) {
                radex_found_t *f = &s_found[s_found_n++];
                memcpy(f->addr, r->bda, sizeof(esp_bd_addr_t));
                f->atype = r->ble_addr_type;
                f->rssi  = r->rssi;
                uint8_t c = nlen < sizeof(f->name) - 1 ? nlen : sizeof(f->name) - 1;
                memcpy(f->name, nm, c);
                f->name[c] = 0;
                ESP_LOGI(TAG, "найден прибор: %s, %02X:%02X:%02X:%02X:%02X:%02X, RSSI %d",
                         f->name, f->addr[0],f->addr[1],f->addr[2],
                         f->addr[3],f->addr[4],f->addr[5], f->rssi);
            }
scan_done:
            break;
        }
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
        case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
            ESP_LOGI(TAG, "локальная приватность установлена, status=%d", param->local_privacy_cmpl.status);
            esp_ble_gattc_app_register(0);
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            // Прибор шлёт свой запрос параметров и не обслуживает ATT, пока
            // согласование не закончилось. В доказанной серии первое чтение
            // уходило именно отсюда, а не по таймеру.
            if (s_connected && param->update_conn_params.status == 0 &&
                s_mtu_state != 1 && s_mtu_state != 2) {
                start_polling("параметры соединения согласованы");
            }
            break;
        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    (void)gatts_if;
    if (event == ESP_GATTS_MTU_EVT) {
        ESP_LOGI(TAG, "прибор согласовал MTU=%d", param->mtu.mtu);
        if (s_mtu_state == 0) {
            s_mtu_state = 3;
        }
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTC_REG_EVT: {
            s_gattc_if = gattc_if;
            uint8_t prev = BTA_GATTC_AutoDiscoverEnable(0);
            ESP_LOGW(TAG, "авто-discovery Bluedroid ВЫКЛЮЧЕН (было %d)", (int)prev);
            if (s_have_target) {
                esp_ble_gattc_open(gattc_if, s_target_addr, s_target_atype, true);
            } else {
                scan_start();      // прибор ещё не выбран — ищем и ждём выбора в Web UI
            }
            break;
        }
        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                /* #RADEX-84: та же строка на каждую попытку заполняла лог целиком.
                   Счётчик неудач ведётся отдельно, здесь достаточно первых
                   нескольких и далее каждой двадцатой. */
                if (s_open_fails <= 3 || (s_open_fails % 20) == 0) {
                    ESP_LOGE(TAG, "ошибка открытия соединения (неудач подряд: %lu)",
                             (unsigned long) s_open_fails);
                }
                break;
            }
            s_conn_id = param->open.conn_id;
            s_connected = 1;
            s_consec_open_fails = 0;   /* #RADEX-145: связь встала — счётчик залипания с нуля */
            s_reconnect_delay_ms = RADEX_RECONNECT_MIN_MS;   /* #RADEX-84: связь есть — пауза с нуля */
            s_open_tick = xTaskGetTickCount();
            s_handle_cursor = 0;
            /* #RADEX-89: связь есть — искать больше нечего. Скан и соединение
               делят одно радио (интервал 0x50 при окне 0x30 — это 60 % времени
               на приём рекламы), и оставленный включённым поиск отнимает эфир у
               уже установленного соединения. В штатном пути скан к этому моменту
               не идёт (цель известна из NVS, открываемся сразу), но после
               «Искать приборы заново» и на первом подключении — идёт. */
            if (s_scanning) {
                esp_ble_gap_stop_scanning();
                s_scanning = false;
                ESP_LOGI(TAG, "поиск в эфире остановлен: прибор подключён");
            }
            ESP_LOGI(TAG, "соединение установлено, conn_id=%d", s_conn_id);
            esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P20);
            break;
        case ESP_GATTC_READ_CHAR_EVT:
            if (param->read.conn_id != s_conn_id) break;
            if (param->read.status == ESP_GATT_OK) {
                s_read_ok++;
                float value;
                // Атрибуция строго по handle ИЗ ОТВЕТА, не по курсору: курсор —
                // наше намерение, а не то, на что прибор реально ответил.
                if (radex_decode(param->read.handle, param->read.value, param->read.value_len, &value)) {
                    radex_on_measurement(param->read.handle, value);
                } else {
                    ESP_LOGW(TAG, "handle 0x%04X: не разобран (len=%d)", param->read.handle, param->read.value_len);
                }
            } else {
                s_read_err++;
                ESP_LOGW(TAG, "чтение handle 0x%04X отказано, status=%d", param->read.handle, param->read.status);
            }
            s_handle_cursor = (s_handle_cursor + 1) % s_handle_count;
            // #RADEX-7: круг закончен (курсор вернулся к началу) — молчим до
            // следующего срока. Раньше здесь безусловно шло issue_read(), и
            // опрос крутился непрерывно.
            if (s_handle_cursor == 0) {
                s_poll_active = 0;
                s_next_poll_tick = xTaskGetTickCount() + pdMS_TO_TICKS(RADEX_POLL_PERIOD_MS);
                ESP_LOGI(TAG, "круг опроса завершён (принято %lu, ошибок %lu) — "
                              "следующий через %d мин",
                         (unsigned long) s_read_ok, (unsigned long) s_read_err,
                         RADEX_POLL_PERIOD_MS / 60000);
            } else {
                issue_read();
            }
            break;
        case ESP_GATTC_CFG_MTU_EVT:
            ESP_LOGI(TAG, "обмен MTU завершён (mtu=%d)", param->cfg_mtu.mtu);
            if (s_mtu_state == 0) {
                s_mtu_state = 3;
            }
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            /* #RADEX-84: разрывом считается только потеря УСТАНОВЛЕННОГО соединения.
               То же событие приходит на каждую неудачную попытку открытия, когда
               прибора нет в эфире, — это отдельный счётчик. */
            int was_connected = s_connected;
            if (was_connected) s_disconnects++; else s_open_fails++;
            if (!was_connected) s_consec_open_fails++;   /* #RADEX-145 */
            s_connected = 0;
            s_mtu_state = 0;
            // #RADEX-7, исправление: круг мог оборваться посреди чтений (прибор
            // отвалился на втором из четырёх). Тогда курсор не возвращался к
            // нулю, флаг «опрос идёт» оставался поднятым — и следующий плановый
            // опрос не запускался НИКОГДА: условие в главном цикле требует
            // !s_poll_active. Плата молча переставала опрашивать прибор, внешне
            // выглядя исправной. Поймано на живой плате 28.08: 1 чтение из 4,
            // дальше тишина. Снимаем флаг на любом разрыве.
            s_poll_active = 0;
            s_handle_cursor = 0;
            TickType_t duration = (xTaskGetTickCount() - s_open_tick) * portTICK_PERIOD_MS;
            if (was_connected) {
                ESP_LOGW(TAG, "разрыв соединения, reason=%d, длительность=%lu мс, разрывы=%lu",
                         param->disconnect.reason, duration, s_disconnects);
            } else {
                /* Не разрыв, а неудачная попытка: печатаем реже, иначе лог платы
                   состоит из одной этой строки и полезное в нём не найти. */
                if (s_open_fails <= 3 || (s_open_fails % 20) == 0) {
                    ESP_LOGW(TAG, "прибор не отвечает на открытие соединения, reason=%d, неудачных попыток=%lu",
                             param->disconnect.reason, s_open_fails);
                }
            }
            /* #RADEX-145: N подряд неудачных open — контроллер, вероятно, залип
               (Cmd Disallowed навсегда). Проверено на живой плате: esp_restart()
               снимает залипание, backoff сам по себе — нет. */
            if (s_consec_open_fails >= RADEX_AUTO_RESTART_AFTER_FAILS) {
                ESP_LOGE(TAG, "%lu подряд неудачных open — перезапуск (подозрение на залипание контроллера)",
                         (unsigned long) s_consec_open_fails);
                esp_restart();
            }
            xSemaphoreGive(s_reconnect_sem);
            break;
        default:
            break;
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_reconnect_sem, portMAX_DELAY);
        /* #RADEX-84: пауза ОБЯЗАТЕЛЬНА до следующей попытки. Без неё контроллер
           получает новую команду открытия раньше, чем закрыл предыдущую, и
           отвечает «Cmd Disallowed» (HCI opcode 0x2043) — попытки не просто
           бесполезны, они мешают друг другу. */
        uint32_t d = s_reconnect_delay_ms;
        vTaskDelay(pdMS_TO_TICKS(d));
        /* #RADEX-86. ПРИЧИНА НЕ УСТАНОВЛЕНА — не считать этот вызов исправлением.

           Достоверно из лога diag-trace-20260829: первая попытка падает на радио
           («hcif disc complete rsn 0x3e», CONN_FAILED_ESTABLISHMENT, через 4,8 с);
           внутри обработки этого события стек САМ шлёт ещё одну команду создания
           соединения (l2c_link.c, ветка retry при 0x3E), и она получает
           «Cmd Disallowed»; дальше КАЖДАЯ попытка отвергается за ~10 мс, уже без
           «disc complete», и так до перезагрузки.

           Чего лог НЕ доказывает: что состояние держит незакрытое создание
           соединения в контроллере. Bluedroid печатает command status только при
           ошибке, успешные команды невидимы, поэтому одинаково возможны:
             — незакрытый инициатор в контроллере;
             — смешение legacy (0x200D) и extended (0x2043) наборов команд, что по
               спецификации обязано давать 0x0C до самого сброса — и косвенно
               подтверждается тем, что варнинг is_aux есть у попыток 2+ и нет у первой;
             — застрявший LCB на стороне хоста.
           Чтобы различить, нужен полный HCI-трейс (CONFIG_BT_HCI_LOG_DEBUG_EN).

           Про сам вызов: проверочный прогон шёл на ЗАВЕДОМО ОТСУТСТВУЮЩЕМ адресе,
           где 0x3E не приходит и ломаться нечему, — то есть менял две переменные
           разом и ничего не доказал. В логе видно «No such connection need to be
           cancelled»: отменять было нечего, вызов оказался пустым. Он стоит после
           паузы бэкоффа, когда попытка давно завершена; чтобы иметь смысл, его
           надо звать сразу по факту неудачи, а не перед следующей. Оставлен как
           безвредный, пока причина не установлена. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        esp_ble_gattc_cancel_open_params_t cp = { .gattc_if = s_gattc_if };
        memcpy(cp.remote_bda, s_target_addr, sizeof(esp_bd_addr_t));
        esp_err_t cancel = esp_ble_gattc_cancel_open(&cp);
        if (cancel != ESP_OK) {
            ESP_LOGD(TAG, "отмена ожидающего открытия: %s", esp_err_to_name(cancel));
        }
        vTaskDelay(pdMS_TO_TICKS(200));   /* дать контроллеру закрыть своё состояние */
#endif
        ESP_LOGI(TAG, "переподключение (пауза была %lu мс)...", (unsigned long) d);
        s_open_tick = xTaskGetTickCount();
        esp_ble_gattc_open(s_gattc_if, s_target_addr, s_target_atype, true);
        d *= 2;
        s_reconnect_delay_ms = (d > RADEX_RECONNECT_MAX_MS) ? RADEX_RECONNECT_MAX_MS : d;
    }
}

static esp_err_t bt_stack_init(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) return ret;

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) return ret;

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

static void security_params_init(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
}

static void tx_power_max(void)
{
    const esp_ble_power_type_t types[] = {ESP_BLE_PWR_TYPE_DEFAULT, ESP_BLE_PWR_TYPE_ADV, ESP_BLE_PWR_TYPE_SCAN};
    for (int i = 0; i < 3; ++i) {
        esp_ble_tx_power_set(types[i], ESP_PWR_LVL_P20);
    }
}

void ble_radex_start(ble_radex_cb_t cb)
{
    s_cb = cb;
    s_have_target = target_load();
    if (s_have_target) {
        ESP_LOGI(TAG, "прибор из NVS: %02X:%02X:%02X:%02X:%02X:%02X (тип адреса %d)",
                 s_target_addr[0], s_target_addr[1], s_target_addr[2],
                 s_target_addr[3], s_target_addr[4], s_target_addr[5], s_target_atype);
    }
    esp_err_t ret;

    ret = bt_stack_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ошибка инициализации стека Bluetooth");
        return;
    }

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gattc_register_callback(gattc_event_handler);

    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gatts_app_register(1);

    esp_ble_gatt_set_local_mtu(247);
    security_params_init();
    tx_power_max();

    s_reconnect_sem = xSemaphoreCreateBinary();
    if (s_reconnect_sem == NULL) {
        ESP_LOGE(TAG, "ошибка создания семафора");
        return;
    }

    xTaskCreate(reconnect_task, "radex_reconnect", 3072, NULL, 5, NULL);

    if (RADEX_LOCAL_PRIVACY) {
        ret = esp_ble_gap_config_local_privacy(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "настройка локальной приватности не удалась");
            esp_ble_gattc_app_register(0);
        }
    } else {
        esp_ble_gattc_app_register(0);
    }

    int tick = 0;
    /* #RADEX-153b: последняя НАПЕЧАТАННАЯ четвёрка. last_conn = -1 — признак
       «ещё ни разу не печатали»: первая же проверка тогда даёт строку с
       исходным состоянием, иначе после старта журнал молчал бы до первого
       события и выглядел бы как неработающий. */
    uint32_t last_ok = 0, last_err = 0, last_disc = 0;
    int      last_conn = -1;
    int      quiet_periods = 0;   // проверок подряд, прошедших без печати
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;
        // Условия проверяются на КАЖДОМ тике 100 мс. Вкладывать их в счётчик
        // печати нельзя: пауза 4000 мс превратилась бы в 10 с и первый запрос
        // мог не успеть уйти до разрыва.
        TickType_t since_open = xTaskGetTickCount() - s_open_tick;
        if (s_connected && s_mtu_state == 3 && since_open >= pdMS_TO_TICKS(RADEX_PRE_READ_MS)) {
            start_polling("пауза после обмена MTU выдержана");
        } else if (s_connected && s_mtu_state == 0 && since_open > pdMS_TO_TICKS(RADEX_MTU_FALLBACK_MS)) {
            s_mtu_state = 2;
            ESP_LOGW(TAG, "прибор про MTU промолчал -> читаем без обмена");
            start_polling("таймаут MTU");
        }
        // #RADEX-7: очередной круг по расписанию. Условие проверяется на каждом
        // тике 100 мс — вкладывать его в счётчик печати нельзя (та же ловушка,
        // что описана выше про паузу MTU).
        if (s_connected && !s_poll_active && s_next_poll_tick != 0 &&
            (int32_t)(xTaskGetTickCount() - s_next_poll_tick) >= 0) {
            start_polling("плановый опрос");
        }
        // #RADEX-153b: проверяем по-прежнему раз в 10 с, но печатаем только при
        // изменении четвёрки либо по heartbeat. Маркер «(без изменений)»
        // отличает heartbeat от строки по событию.
        if (tick % RADEX_STATUS_CHECK_TICKS == 0) {
            uint32_t ok = s_read_ok, err = s_read_err, disc = s_disconnects;
            int  conn = s_connected ? 1 : 0;
            bool changed = (ok != last_ok) || (err != last_err) ||
                           (disc != last_disc) || (conn != last_conn);
            quiet_periods++;
            if (changed || quiet_periods >= RADEX_STATUS_HEARTBEAT_N) {
                ESP_LOGI(TAG, "статус: чтений=%lu ошибок=%lu разрывов=%lu связь=%s%s",
                         (unsigned long) ok, (unsigned long) err,
                         (unsigned long) disc, conn ? "есть" : "нет",
                         changed ? "" : " (без изменений)");
                last_ok = ok; last_err = err; last_disc = disc; last_conn = conn;
                quiet_periods = 0;
            }
        }
    }
}

// ── Диагностические геттеры (для /api/status) ─────────────────────────────
bool     ble_radex_connected(void)   { return s_connected != 0; }
uint32_t ble_radex_reads_ok(void)    { return s_read_ok; }
uint32_t ble_radex_read_errors(void) { return s_read_err; }
uint32_t ble_radex_disconnects(void) { return s_disconnects; }
uint32_t ble_radex_open_fails(void) { return s_open_fails; }
