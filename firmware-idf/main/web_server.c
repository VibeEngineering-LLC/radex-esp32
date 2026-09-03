// ══════════════════════════════════════════════════════════════════════════
//  web_server.c — HTTP-морда шлюза: показания, статус, лог.
//
//  Взят каркас донора (atomspectra-waterfall/main/web_server.c): та же схема
//  «httpd_start + статическая таблица uris[] + EMBED_HTML_HANDLER». Тело
//  обработчиков своё — у донора 50 URI под спектры и водопад, здесь нужно 5.
//
//  max_uri_handlers считается от своей таблицы: превышение лимита НЕ даёт
//  ошибки, а тихо роняет регистрацию последних обработчиков (авария донора,
//  зафиксирована в его комментарии). Держим запас и проверяем при добавлении.
// ══════════════════════════════════════════════════════════════════════════
#include "net_config.h"
#include "radex_data.h"
#include "ble_radex.h"
#include "ha_mqtt.h"
#include "log_ring.h"
#include "narodmon.h"
#include "radon_stats.h"
#include "poll_cycle.h"
#include "http_io_gate.h"   /* #RADEX-171: полоса тяжёлых файловых запросов */
#include <nvs.h>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "net_time.h"      /* #RADEX-113: источник и состояние времени */
#include <esp_wifi.h>
#include <esp_bt.h>              // мощность передатчика BLE — спрашиваем у чипа
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>   /* isnan в разборе истории */


static const char *TAG = "web";

/* #RADEX-170. ЕСЛИ ВЫ СОБИРАЕТЕСЬ ДОБАВИТЬ ASYNC-ОБРАБОТЧИК — СНАЧАЛА СЮДА.
   Восемь обработчиков ниже держат буферы ответа в `static char` (~30 КБ):
   handle_history, handle_log, handle_log_download, handle_stats, handle_assess,
   handle_cycle, handle_export_body, handle_history_range_body. На стеке задачи
   httpd им места нет — cfg.stack_size = 6144.
   `static` безопасен ровно по одной причине: esp_http_server обслуживает все
   соединения ОДНОЙ задачей в цикле select(), а асинхронных обработчиков в
   проекте нет (httpd_req_async_handler_begin не вызывается нигде — проверено
   грепом 01.09.2026). Два обработчика физически не могут идти одновременно,
   делить буфер не с кем. Второй httpd в прошивке есть (captive-портал,
   wifi_manager.c), но он живёт ТОЛЬКО в NET_MODE_SETUP, когда этот сервер не
   запускается вовсе (см. проверку wifi_manager_mode() в main.c).
   Как только появится httpd_req_async_handler_begin(), второй httpd на этих
   же обработчиках или своя задача, их дёргающая, — КАЖДЫЙ буфер станет гонкой.
   Проявится она не отказом, а перемешанными ответами: запрос увидит куски
   чужого JSON. Перед таким шагом перевести буферы на heap_caps_malloc(
   MALLOC_CAP_SPIRAM)+free (образец — handle_tests_list) либо на стек, подняв
   cfg.stack_size. Сейчас динамику не вводим намеренно: 30 КБ malloc/free на
   запрос — фрагментация ради проблемы, которой при однопоточном сервере нет.
   Это НЕ про гонку с задачей BLE — та отдельная, см. #RADEX-172. */

#define EMBED_HTML_HANDLER(fn, sym)                                          \
    static esp_err_t fn(httpd_req_t *req) {                                  \
        extern const uint8_t sym##_start[] asm("_binary_" #sym "_start");    \
        extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");      \
        httpd_resp_set_type(req, "text/html");                               \
        httpd_resp_send(req, (const char *)sym##_start,                      \
                        sym##_end - sym##_start);                            \
        return ESP_OK;                                                       \
    }

EMBED_HTML_HANDLER(handle_root, index_html)

static esp_err_t handle_data(httpd_req_t *req)
{
    char buf[512];
    int n = radex_data_json(buf, sizeof(buf));
    if (n < 0 || n >= (int)sizeof(buf)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// История показаний для графика. Буфер под HIST_N точек по ~70 символов;
// отдаём одним куском — на 120 точках это ~8 КБ, дробить незачем.
static esp_err_t handle_history(httpd_req_t *req)
{
    static char buf[10240];          // static: 10 КБ на стеке httpd не поместятся   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int n = log_ring_hist_json(buf, sizeof(buf));
    if (n < 0) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// Лог платы в браузер: тот самый недостающий канал, из-за которого причину
// отказа приходилось искать по UART с ребутом платы при открытии порта.
/* #RADEX-101/105: общий фильтр пользовательского префикса имени файла — жёсткий
   белый список (латиница, цифры, дефис, подчёркивание), потому что префикс уходит
   прямо в HTTP-заголовок Content-Disposition: перевод строки там позволил бы
   подделать ответ целиком, кавычка — разорвать filename. Раньше цикл был вписан
   ОДИН раз в handle_export; #RADEX-105 добавляет второй потребитель (лог платы),
   и дублировать фильтр безопасности — конкретно ту вещь, которую нельзя разъехаться
   молча между копиями. */
static void extract_prefix(httpd_req_t *req, char *out, size_t out_sz)
{
    out[0] = 0;
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return;
    char raw[80];
    if (httpd_query_key_value(q, "prefix", raw, sizeof(raw)) != ESP_OK) return;
    size_t n = 0;
    for (size_t i = 0; raw[i] && n < out_sz - 1; i++) {
        char c = raw[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out[n++] = c;
    }
    out[n] = 0;
}

static esp_err_t handle_log(httpd_req_t *req)
{
    static char buf[8192];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int n = log_ring_dump(buf, sizeof(buf));
    if (n < 0) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, buf, n);
}

/* #RADEX-105, оператор: «должен быть импорт лога с префиксом и сброс». Прежний
   /api/log отдавал text/plain без Content-Disposition — открывался прямо в
   вкладке браузера вместо скачивания, а имени файла не было вовсе. Здесь тот же
   дамп кольца, но с заголовком attachment и тем же префиксом, что у CSV: один
   набор файлов от одного прибора узнаётся по общему префиксу в имени. */
static esp_err_t handle_log_download(httpd_req_t *req)
{
    static char buf[8192];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int n = log_ring_dump(buf, sizeof(buf));
    if (n < 0) return httpd_resp_send_500(req);

    char prefix[33];
    extract_prefix(req, prefix, sizeof(prefix));
    char disp[96];
    if (prefix[0]) {
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s-radex-log.txt\"", prefix);
    } else {
        snprintf(disp, sizeof(disp), "attachment; filename=\"radex-log.txt\"");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    return httpd_resp_send(req, buf, n);
}

/* #RADEX-105: сброс лога по кнопке. Разрушает только диагностический буфер в
   ОЗУ (не историю показаний на флеш и не настройки) — того же класса действие,
   что и «Очистить» на вкладке «Графики», подтверждение просит сама страница. */
static esp_err_t handle_log_clear(httpd_req_t *req)
{
    log_ring_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Народмон (#RADEX-8) ──────────────────────────────────
// Включение действует ТОЛЬКО на текущий сеанс работы платы: признак
// «включено» не сохраняется, любая перезагрузка возвращает выгрузку
// в выключенное состояние (см. шапку narodmon.h).
// Включение NarodMon действует только на текущий сеанс работы платы и не переживает перезагрузку
static esp_err_t handle_nm_get(httpd_req_t *req)
{
    char buf[320];
    int len = narodmon_config_json(buf, sizeof(buf));
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_nm_set(httpd_req_t *req)
{
    char body[96];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    char *sep = strchr(body, '|');
    int interval = 10;
    if (sep != NULL) {
        *sep = '\0';
        interval = atoi(sep + 1);
    }

    if (!narodmon_set_config(body, interval)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_nm_enable(httpd_req_t *req)
{
    char body[8];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    bool enable = (body[0] == '1');
    bool result = narodmon_set_enabled(enable);

    // След в журнале нужен в обе стороны: отправка данных наружу и её
    // прекращение одинаково важны для разбора постфактум.
    ESP_LOGW(TAG, "выгрузка на narodmon.ru %s%s",
             enable ? "ВКЛЮЧЕНА" : "выключена",
             enable ? " (действует только до перезагрузки платы)" : "");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, result ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── История и оценка соответствия (#RADEX-11, #RADEX-12) ───────────
// Методика — рациональный метод (АНРИ 2025), см. шапку radon_stats.c.
static esp_err_t handle_stats(httpd_req_t *req) {
    int32_t crl = 200; // норматив для новых и реконструированных зданий, Бк/м3 по ОА
    /* 30, а не 20: паспортные 20 % Radex — заводская спецификация при высокой
   концентрации и длительном наборе. Метрологические испытания партии бытовых
   мониторов дают до 30 % (k=2), и это значение рекомендуется брать без
   собственных данных поверки. #RADEX-91/#RADEX-135: «ниже 20 методика не
   разрешает» — выдуманная формулировка (W-057), источник §4.1.7 говорит
   «нецелесообразно снижать», не «запрещено»; клампом ниже (radon_stats.c,
   "Исправляем u_d, если слишком маленькое") 20% остаётся МИНИМУМОМ приложения,
   а не запретом методики. Этот дефолт (30) НЕЗАВИСИМ от клиентского дефолта
   20% у #device-uncertainty (web/index.src.html) — тот вообще не хранится
   в NVS, это два разных числа для двух разных величин, не общий дефолт с
   переопределением. */
    int32_t ud = 30;
    int8_t restr = 0;  // режим вентиляции: 0 — обычная эксплуатация

    // Отсутствие сохранённых настроек — НОРМА, а не отказ: на чистой плате
    // пространства имён ещё нет. Прежняя версия отдавала на это 500, и вся
    // вкладка оценки была пуста до первого сохранения настроек.
    nvs_handle_t h;
    if (nvs_open("radon", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "crl", &crl);
        nvs_get_i32(h, "ud", &ud);
        nvs_get_i8(h, "restr", &restr);
        nvs_close(h);
    }

    static char buf[1536];   /* static: 1.5 КБ на стеке httpd не помещаются */   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int len = radon_stats_json(buf, sizeof(buf), (float)crl, ud / 100.0f, restr != 0);
    if (len < 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}


/* GET /api/assess?from=<эпоха>&to=<эпоха> — заключение по выбранному интервалу
   истории (#RADEX-40). Границы необязательны: ноль означает «без ограничения».
   Настройки норматива и неопределённости берутся те же, что и для /api/stats —
   иначе одно и то же измерение получало бы разные заключения. */
static esp_err_t handle_assess(httpd_req_t *req) {
    time_t from = 0, to = 0;
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[32];
        if (httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) from = (time_t)atoll(val);
        if (httpd_query_key_value(query, "to", val, sizeof(val)) == ESP_OK)   to   = (time_t)atoll(val);
    }

    int32_t crl = 200, ud = 30;
    int8_t restr = 0;
    nvs_handle_t h;
    if (nvs_open("radon", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "crl", &crl);
        nvs_get_i32(h, "ud", &ud);
        nvs_get_i8(h, "restr", &restr);
        nvs_close(h);
    }

    static char buf[512];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int len = radon_stats_assess_json(buf, sizeof(buf), from, to,
                                      (float)crl, ud / 100.0f, restr != 0);
    if (len < 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}


/* GET /api/cycle — определённый цикл измерений прибора (#RADEX-19). Модуль
   определения был написан, но наружу не выводился: проверить его работу на
   живых данных было НЕЧЕМ. Механизм, чью работу нельзя наблюдать, нельзя и
   принять — отсюда маршрут. */
static esp_err_t handle_cycle(httpd_req_t *req) {
    static char buf[192];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int len = poll_cycle_json(buf, sizeof(buf));
    if (len < 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}


/* POST /api/compact — принудительное уплотнение истории (#RADEX-59). Нужен не
   только оператору: без него правильность агрегации нельзя проверить на живых
   данных, а «проверено, что работает» без прогона ничего не значит.
   Параметры raw и hour (сутки) — необязательные, умолчания 30 и 365. */
static esp_err_t handle_compact(httpd_req_t *req) {
    uint32_t raw = 30, hour = 365;
    char query[96], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "raw", val, sizeof(val)) == ESP_OK)  raw  = (uint32_t)atoi(val);
        if (httpd_query_key_value(query, "hour", val, sizeof(val)) == ESP_OK) hour = (uint32_t)atoi(val);
    }
    if (hour < raw) hour = raw;      /* среднечасовые не могут кончаться раньше сырых */

    int rows = radon_stats_compact(raw, hour);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%s,\"rows\":%d,\"raw_days\":%u,\"hour_days\":%u}",
                     rows >= 0 ? "true" : "false", rows, (unsigned)raw, (unsigned)hour);
    if (n < 0 || n >= (int)sizeof(buf)) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* POST /api/reboot - программный перезапуск платы (#RADEX-148, оператор:
   "кнопку перезагрузка сделай").

   Не косметика: по разбору #RADEX-145 программный esp_restart() снимает
   залипание BLE-контроллера, то есть это штатный способ восстановить связь с
   прибором, не имея физического доступа к плате.

   esp_restart() НЕЛЬЗЯ звать прямо в обработчике: httpd_resp_send() лишь
   отдаёт данные сокету, доставки к этому моменту ещё нет - перезапуск в той
   же функции оборвал бы соединение до того, как браузер получит ответ.
   Поэтому ответ уходит первым, а перезапуск отложен на 500 мс одноразовым
   esp_timer (его callback исполняется в задаче таймеров, вне HTTP-стека).

   Код сгенерирован по спеке scripts/ollama/spec_reboot_handler.md
   (IRON MODE §31.B, ступень 2 - Ollama qwen3-coder:30b). */
static void reboot_timer_cb(void *arg) {
    (void)arg;
    esp_restart();
}

static esp_err_t handle_reboot(httpd_req_t *req) {
    static esp_timer_handle_t s_reboot_timer = NULL;
    esp_err_t err;

    if (s_reboot_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = reboot_timer_cb,
            .arg = NULL,
            .name = "reboot"
        };
        err = esp_timer_create(&timer_args, &s_reboot_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ошибка создания таймера перезагрузки: %s", esp_err_to_name(err));
            s_reboot_timer = NULL;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    if (esp_timer_is_active(s_reboot_timer)) {
        esp_timer_stop(s_reboot_timer);
    }

    err = esp_timer_start_once(s_reboot_timer, 500000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ошибка запуска таймера перезагрузки: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "перезагрузка по запросу с веб-страницы через 500 мс");

    httpd_resp_set_type(req, "application/json");
    static const char resp[] = "{\"ok\":true,\"delay_ms\":500}";
    return httpd_resp_send(req, resp, sizeof(resp) - 1);
}

static esp_err_t handle_stats_set(httpd_req_t *req) {
    char body[64];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';

    int crl, ud, restr;
    if (sscanf(body, "%d|%d|%d", &crl, &ud, &restr) != 3) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Проверка диапазонов
    if (crl < 50 || crl > 1000) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (ud < 20 || ud > 50) { // Ниже 20% не рекомендуется по методике из-за межгодовых колебаний
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (restr != 0 && restr != 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("radon", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    nvs_set_i32(h, "crl", crl);
    nvs_set_i32(h, "ud", ud);
    nvs_set_i8(h, "restr", restr);
    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Applied settings: crl=%d ud=%d restr=%d", crl, ud, restr);

    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

/* #RADEX-113: состояние службы времени и её настройка. GET отдаёт всё, что
   нужно человеку, чтобы понять, доверять ли меткам истории: синхронизировано ли
   время, откуда оно взято и какое сейчас по местному поясу. */
extern const char *net_time_ntp_host(void);
extern const char *net_time_tz(void);
extern void net_time_load_config(void);
extern int radon_stats_rebase(time_t now);   /* #RADEX-113: пересчёт относительных меток */

static esp_err_t handle_ntp_get(httpd_req_t *req) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);

    const char *src = "нет";
    switch (net_time_source()) {
        case TIME_SRC_SNTP:    src = "NTP"; break;
        case TIME_SRC_BROWSER: src = "браузер"; break;
        case TIME_SRC_MANUAL:  src = "вручную"; break;
        default: break;
    }

    char buf[288];
    int n = snprintf(buf, sizeof(buf),
        "{\"host\":\"%s\",\"tz\":\"%s\",\"synced\":%s,\"source\":\"%s\","
        "\"now\":%lld,\"local\":\"%s\"}",
        net_time_ntp_host(), net_time_tz(),
        net_time_sntp_synced() ? "true" : "false", src,
        (long long) now, stamp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t handle_ntp_set(httpd_req_t *req) {
    char body[128];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[ret] = 0;

    /* Формат «host|tz». Пустой host запрещён: без сервера служба молча
       перестала бы синхронизироваться, а на странице выглядела бы настроенной. */
    char host[64] = {0}, tz[48] = {0};
    const char *bar = strchr(body, '|');
    if (!bar || bar == body) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"host\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t hl = (size_t)(bar - body);
    if (hl >= sizeof(host)) hl = sizeof(host) - 1;
    memcpy(host, body, hl);
    strncpy(tz, bar + 1, sizeof(tz) - 1);

    nvs_handle_t h;
    if (nvs_open("radon", NVS_READWRITE, &h) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }
    nvs_set_str(h, "ntp_host", host);
    nvs_set_str(h, "tz", tz[0] ? tz : "UTC0");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* Пояс применяем сразу — он не требует перезапуска сети. Сервер NTP
       подхватится при следующем старте: менять его на лету значит гасить
       работающую службу ради настройки, которая меняется раз в жизни. */
    net_time_load_config();
    setenv("TZ", tz[0] ? tz : "UTC0", 1);
    tzset();

    ESP_LOGI(TAG, "NTP: сервер=%s пояс=%s", host, tz);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"restart_needed\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* #RADEX-113: ручной ввод времени пользователем. Формат тела — десятичный
   epoch (секунды UTC) или ISO "YYYY-MM-DDTHH:MM:SS" в местном поясе платы.
   После установки времени вызываем radon_stats_rebase() — записи, накопленные
   до синхронизации с относительными метками, разово получают реальный epoch и
   входят в историю на общих основаниях (иначе они висели бы отдельным классом
   и в статистику попасть не могли).
   Guard net_time_should_accept() уже отбрасывает манипуляции при активном SNTP:
   война источников времени приводит к скачкам меток. */
static esp_err_t handle_time_set(httpd_req_t *req) {
    char body[64];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[ret] = 0;

    time_t new_time = 0;
    /* Первая попытка — десятичный epoch. Простой формат, годится и для полей
       и для скриптов. */
    char *end = NULL;
    long long v = strtoll(body, &end, 10);
    if (end != body && v > 1700000000LL && v < 4000000000LL) {
        new_time = (time_t)v;
    } else {
        /* Вторая попытка — ISO "YYYY-MM-DDTHH:MM:SS" в местном поясе.
           datetime-local в браузере отдаёт ровно этот формат. */
        struct tm tm = {0};
        int y, mo, d, hh, mm, ss = 0;
        int got = sscanf(body, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss);
        if (got >= 5 && y >= 2024) {
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
            tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss;
            tm.tm_isdst = -1;
            new_time = mktime(&tm);
        }
    }
    if (new_time <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"bad_time\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Отклоняем при активном SNTP: два источника времени, идущих независимо,
       гарантированно расходятся. Ручное значение имеет смысл только пока
       автосинхронизация не сработала. */
    time_t before = time(NULL);
    int64_t dt = (int64_t)llabs((long long)new_time - (long long)before);
    if (!net_time_should_accept(net_time_sntp_synced(), true, dt)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"sntp_active\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    struct timeval tv = { .tv_sec = new_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    net_time_set_source(TIME_SRC_MANUAL);

    int converted = radon_stats_rebase(new_time);
    ESP_LOGI(TAG, "Ручной ввод времени: %lld, переведено записей: %d",
             (long long)new_time, converted);

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"time\":%lld,\"rebased\":%d}",
             (long long)new_time, converted);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* #RADEX-171/172: тело вынесено в отдельную функцию НАМЕРЕННО. Обёртка ниже
   берёт gate и мьютекс и отпускает их ровно один раз, каким бы из своих
   ветвлений тело ни вышло. Разложить enter/leave по всем return'ам тела было
   бы тем же самым лишь до первой новой ветки: забытый leave навсегда закрывает
   тяжёлую полосу для ВСЕХ запросов, и проявляется это не сразу. */
static esp_err_t handle_export_body(httpd_req_t *req) {
    const char *fname = radon_stats_file();
    FILE *f = fopen(fname, "r");
    if (!f) {
        httpd_resp_send_404(req);
        httpd_resp_send(req, "история пока пуста", -1);
        return ESP_OK;
    }

    /* #RADEX-101: имя файла начинается с пользовательского префикса — на одном
       компьютере оказываются выгрузки с нескольких приборов и за разные периоды,
       и десяток одинаковых radon-history.csv различить нельзя.

       Префикс уходит прямо в HTTP-заголовок, поэтому фильтруется жёстко и по
       БЕЛОМУ списку: латиница, цифры, дефис и подчёркивание. Всё остальное —
       кириллица, пробелы, точки, кавычки — отбрасывается. Причина не в
       придирчивости: перевод строки в заголовке позволил бы подделать ответ
       целиком (инъекция заголовка), а кавычка — разорвать filename. Длина
       ограничена 32 символами. */
    char prefix[33];
    extract_prefix(req, prefix, sizeof(prefix));

    char disp[96];
    if (prefix[0]) {
        snprintf(disp, sizeof(disp),
                 "attachment; filename=\"%s-radon-history.csv\"", prefix);
    } else {
        snprintf(disp, sizeof(disp), "attachment; filename=\"radon-history.csv\"");
    }

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    static char chunk[1024]; /* тот же довод: стек задачи веб-сервера мал */   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    size_t bytes_read;
    while ((bytes_read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, chunk, bytes_read);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // завершение
    return ESP_OK;
}

/* Выгрузка всего файла истории — самый тяжёлый запрос платы. Ждём слот до 3 с,
   а не отказываем сразу: выгрузку запускает человек кнопкой, повторить её
   некому, и подождать секунду ему лучше, чем получить 503. */
static esp_err_t handle_export(httpd_req_t *req) {
    if (!http_io_gate_enter_wait_or_503(req, 3000)) return ESP_OK;   /* 503 уже отправлен */
    /* #RADEX-172: мьютекс здесь НЕ держим — см. подробное обоснование в
       radon_stats.h. Коротко: экспорт файла 188 КБ на RSSI -88 дБм измерен в
       ~14 с, дольше таймаута писателя, и показание терялось. Дозапись
       читателю не мешает, поэтому сверяем ПОКОЛЕНИЕ файла: если за время
       отдачи файл пересобрали (прореживание, compact, reset), ответ битый и
       его надо оборвать, а не досылать вперемешку. */
    uint32_t gen0 = radon_stats_generation();
    esp_err_t rc = handle_export_body(req);
    if (rc == ESP_OK && radon_stats_generation() != gen0) {
        ESP_LOGW(TAG, "#RADEX-172: файл истории пересобран во время экспорта, ответ оборван");
        rc = ESP_FAIL;   /* соединение закроется, клиент увидит обрыв, а не склейку */
    }
    http_io_gate_leave();
    return rc;
}


// #RADEX-16: управление тестом по рациональному методу.
// POST /api/test телом "start" — отметить начало теста, "reset" — стереть историю.
// Разделены намеренно: старт лишь ставит точку отсчёта и данные сохраняет,
// сброс уничтожает их безвозвратно.
static esp_err_t handle_test_ctl(httpd_req_t *req)
{
    char body[16] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (httpd_req_recv(req, body, len) <= 0) return httpd_resp_send_500(req);
    body[len] = 0;

    bool ok = false;
    const char *err = "";
    if (strcmp(body, "start") == 0) {
        ok = radon_stats_start_test();
        if (!ok) err = "часы платы ещё не синхронизированы";
        ESP_LOGW(TAG, "замер по рациональному методу: старт %s", ok ? "принят" : "ОТКЛОНЁН");
    } else if (strcmp(body, "reset") == 0) {
        ok = radon_stats_reset();
        if (!ok) err = "хранилище недоступно";
        ESP_LOGW(TAG, "история показаний СТЁРТА оператором (%s)", ok ? "успешно" : "ошибка");
    } else {
        err = "неизвестная команда";
    }

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%s,\"error\":\"%s\",\"start\":%lld}",
                     /* #RADEX-150: то же ЭФФЕКТИВНОЕ начало, что и в /api/stats.
                        После «Сброса» явная отметка снята, но замер продолжает
                        идти по накопленной истории — вернуть здесь 0 значило бы
                        отдать странице «замер не начат», расходящееся с /api/stats
                        уже в следующем её тике. */
                     ok ? "true" : "false", err, (long long) radon_stats_test_start_eff());
    if (n < 0 || n >= (int)sizeof(buf)) return httpd_resp_send_500(req);
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>


/* #RADEX-171/172: тело + обёртка, довод тот же, что у handle_export. Здесь
   ветвлений с ранним return четыре (файл не открылся дважды, пустой интервал,
   успех) — ровно тот случай, где ручная расстановка leave отказывает. */
static esp_err_t handle_history_range_body(httpd_req_t *req)
{
    // Буфер для строки из файла (128 байт)
    static char line[128];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    
    /* Параметры запроса. Разбор ОБЯЗАН идти по коду возврата, а не по длине
       буфера, и в СВОЙ буфер, а не в общий с чтением файла. Прежняя версия
       делала ровно наоборот: при запросе «?from=…&max=300» отсутствующий
       параметр оставлял в статическом буфере значение от ПРОШЛОГО запроса,
       from становился меткой последнего измерения — и обработчик отдавал одну
       запись из двадцати семи, а график на странице оставался пустым (#RADEX-48).
       Тип long long, а не int: метка эпохи перестанет помещаться в int в 2038. */
    char query_str[128];
    char val[32];
    long long from = 0;
    long long to = 0;
    int max = 300;

    if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) == ESP_OK) {
        if (httpd_query_key_value(query_str, "from", val, sizeof(val)) == ESP_OK) {
            from = atoll(val);
            if (from < 0) from = 0;
        }
        if (httpd_query_key_value(query_str, "to", val, sizeof(val)) == ESP_OK) {
            to = atoll(val);
            if (to < 0) to = 0;
        }
        if (httpd_query_key_value(query_str, "max", val, sizeof(val)) == ESP_OK) {
            max = atoi(val);
            if (max < 50) max = 50;
            if (max > 1000) max = 1000;
        }
    }
    
    // Открываем файл
    FILE *f = fopen(radon_stats_file(), "r");
    if (!f) {
        // Файл не существует или не открылся — возвращаем пустой ответ
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"rows\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    /* #RADEX-174: первый проход считает ДВЕ разные величины.
       rows — сколько СТРОК файла попало в интервал: по ним и только по ним
       считается прореживание и отсечка второго прохода, потому что отдаём мы
       строки. total — сколько ИЗМЕРЕНИЙ эти строки представляют (сумма весов
       шестого поля): именно это число сверяется с /api/stats storage.points и
       показывается человеку. До правки величина была одна, и после уплотнения
       истории страница сообщала «5 точек» там, где измерений был 71. */
    int rows = 0;
    long total = 0;
    radon_row_t row;

    while (fgets(line, sizeof(line), f)) {
        if (radon_csv_parse(line, &row)) {
            // Проверяем попадание в интервал
            if (row.ts >= from && (to == 0 || row.ts <= to)) {
                rows++;
                total += (long)row.w;
            }
        }
    }
    
    // Закрываем файл после первого прохода
    fclose(f);
    
    // Если нет подходящих строк, возвращаем пустой ответ
    if (rows == 0) {
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"rows\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    // Вычисляем шаг прореживания — по СТРОКАМ, их и прореживаем
    int step = 1;
    if (rows > max) {
        step = rows / max;
        if (step < 1) step = 1;
    }
    
    // Открываем файл снова для второго прохода
    f = fopen(radon_stats_file(), "r");
    if (!f) {
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"rows\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    // Буфер для отправки частей ответа
    static char chunk[1024];   /* #RADEX-170: static безопасен только пока httpd однопоточный, см. шапку файла */
    int chunk_len = 0;
    
    // Начинаем формировать JSON-ответ
    chunk_len = snprintf(chunk, sizeof(chunk),
        "{\"total\":%ld,\"rows\":%d,\"step\":%d,\"points\":[", total, rows, step);
    
    // Отправляем начальную часть ответа
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, chunk, chunk_len);
    
    int count = 0;
    int sent = 0;   /* сколько точек реально отдано — это и есть n */
    int send_first = 1;  // Флаг для отправки первой точки
    
    while (fgets(line, sizeof(line), f)) {
        /* #RADEX-172: второй проход НИКОГДА не отдаёт больше, чем насчитал
           первый. Мьютекс на оба прохода мы не держим (он starve'ит писателя,
           измерено), поэтому между проходами задача BLE успевает дописать
           строки — и без этой отсечки ответ содержал бы n > total, то есть сам
           себе противоречил. Отсечка делает ответ согласованным по построению,
           а не по удаче: лишние свежие точки просто приедут следующим запросом. */
        if (sent >= rows) break;
        if (radon_csv_parse(line, &row)) {
            long long ts = (long long)row.ts;
            float radon = row.radon, temp = row.temp, hum = row.hum;

            // Проверяем попадание в интервал
            if (ts >= from && (to == 0 || ts <= to)) {
                // Если нужно прореживать и шаг не подошел, пропускаем
                if (rows > max && count % step != 0 && count != 0 && count != rows - 1) {
                    count++;
                    continue;
                }

                // Отправляем первую точку всегда
                if (count == 0 || send_first) {
                    send_first = 0;
                } else if (rows > max && count % step != 0) {
                    count++;
                    continue;
                }

                /* Значения храним во float: приведение к int теряло дробную
                   часть (радон 127 вместо 127.22). Маркеры «не измерено»
                   выносим за пределы физически возможных значений. */
                float r_f = radon;
                float c_f = temp;
                float h_f = hum;
                if (isnan(radon)) r_f = -1.0f;
                if (isnan(temp))  c_f = -100.0f;
                if (isnan(hum) || hum == 255.0f) h_f = -1.0f;
                /* Единое форматирование вместо каскада из восьми вариантов:
                   тот терял дробную часть (радон 127 вместо 127.22), потому что
                   значения приводились к int. Числа собираем строками, чтобы
                   «не измерено» отдавалось как null, а не как 0 или -1. */
                char rs[16], cs[16], hs[16];
                if (r_f < 0 || isnan(r_f)) snprintf(rs, sizeof(rs), "null");
                else                       snprintf(rs, sizeof(rs), "%.2f", r_f);
                if (c_f < -90 || isnan(c_f)) snprintf(cs, sizeof(cs), "null");
                else                         snprintf(cs, sizeof(cs), "%.1f", c_f);
                if (h_f < 0 || h_f > 100 || isnan(h_f)) snprintf(hs, sizeof(hs), "null");
                else                                    snprintf(hs, sizeof(hs), "%.0f", h_f);
                /* #RADEX-174: вес точки отдаётся всегда. Страница считает по
                   этим точкам среднее и погрешность; без веса суточная средняя
                   из 25 измерений весила бы столько же, сколько одиночная
                   точка, и среднее по «Всему времени» разошлось бы с тем, что
                   плата отдаёт в /api/stats. */
                int point_len = snprintf(chunk, sizeof(chunk),
                    "{\"t\":%lld,\"r\":%s,\"c\":%s,\"h\":%s,\"w\":%u}",
                    ts, rs, cs, hs, (unsigned)row.w);
                
                /* Запятую ставим ПЕРЕД точкой, начиная со второй отданной.
                   Прежнее условие «не последняя входная» ломало ответ при
                   прореживании: если последняя входная точка отсеяна, запятая
                   оставалась после последней отданной — «...},]}» это уже не
                   JSON, и график молча оставался пустым. */
                if (sent > 0) {
                    httpd_resp_send_chunk(req, ",", 1);
                }
                httpd_resp_send_chunk(req, chunk, point_len);
                sent++;
                count++;
            }
        }
    }
    
    // Закрываем файл
    fclose(f);
    
    // Завершаем JSON-ответ
    /* n отдаём в конце: до выдачи точное число отданных точек неизвестно,
       и раньше в заголовок писался ноль. Порядок полей в JSON значения не имеет. */
    chunk_len = snprintf(chunk, sizeof(chunk), "],\"n\":%d}", sent);
    httpd_resp_send_chunk(req, chunk, chunk_len);
    
    // Отправляем завершающий NULL-чанк
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

/* Запрос графика — интерактивный: страница дёргает его при каждой смене
   диапазона. Ждём слот дольше, чем у export (5 с): мгновенный 503 здесь
   означает пустой график у человека, который ничего плохого не сделал —
   просто в этот момент кто-то качал историю. */
static esp_err_t handle_history_range(httpd_req_t *req)
{
    if (!http_io_gate_enter_wait_or_503(req, 5000)) return ESP_OK;   /* 503 уже отправлен */
    /* Мьютекс не держим — довод тот же, что у handle_export. Согласованность
       ответа обеспечена отсечкой sent >= total внутри тела, а пересборку файла
       ловим по поколению. */
    uint32_t gen0 = radon_stats_generation();
    esp_err_t rc = handle_history_range_body(req);
    if (rc == ESP_OK && radon_stats_generation() != gen0) {
        ESP_LOGW(TAG, "#RADEX-172: файл пересобран во время выдачи графика");
        rc = ESP_FAIL;
    }
    http_io_gate_leave();
    return rc;
}

/* #RADEX-102: список сохранённых замеров для карточки на «Графиках». Буфер —
   из PSRAM (heap_caps_malloc), список может вырасти с числом замеров, а
   стек задачи веб-сервера уже урезан (см. cfg.stack_size ниже). */
static esp_err_t handle_tests_list(httpd_req_t *req)
{
    size_t cap = 8192;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);
    if (!buf) return httpd_resp_send_500(req);

    int n = radon_stats_tests_list_json(buf, cap);
    if (n < 0) { free(buf); return httpd_resp_send_500(req); }

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, buf, n);
    free(buf);
    return e;
}

/* GET /api/tests/points?start=<epoch> — точки ОДНОГО замера, тот же формат,
   что /api/history/range (страница переиспользует парсер точек). start —
   query-параметр, не сегмент пути: остальные маршруты с параметром в этом
   проекте (history/range, assess, compact) везде принимают строку запроса,
   отдельный стиль под один маршрут заводить незачем. */
static esp_err_t handle_tests_points_body(httpd_req_t *req)
{
    char query[64], val[24];
    long long start = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "start", val, sizeof(val)) == ESP_OK) start = atoll(val);
    }
    if (start <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "нужен ?start=<epoch>");

    char tf[48];
    radon_stats_test_file((time_t)start, tf, sizeof(tf));
    FILE *chk = tf[0] ? fopen(tf, "r") : NULL;
    if (!chk) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "замер не найден");
    fclose(chk);
    size_t cap = 32768;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(cap);
    if (!buf) return httpd_resp_send_500(req);

    int n = radon_stats_test_points_json((time_t)start, buf, cap);
    if (n < 0) { free(buf); return httpd_resp_send_500(req); }

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, buf, n);
    free(buf);
    return e;
}

/* #RADEX-171/172: тот же интерактивный класс, что history/range. Мьютекс нужен
   и сверх модульного: тело само делает fopen(tf) в обход radon_stats.c. */
static esp_err_t handle_tests_points(httpd_req_t *req)
{
    if (!http_io_gate_enter_wait_or_503(req, 5000)) return ESP_OK;
    /* Здесь мьютекс тоже не держим через отправку: тело собирает ответ целиком
       в буфер вызовом radon_stats_test_points_json(), а тот берёт мьютекс сам
       и отпускает ДО того, как байты уйдут в сеть. Держать его ещё и снаружи
       значило бы вернуть ту самую блокировку писателя на время передачи. */
    esp_err_t rc = handle_tests_points_body(req);
    http_io_gate_leave();
    return rc;
}

/* #RADEX-174: приём истории извне. */
#define RADON_IMPORT_MAX_BYTES 262144

static esp_err_t handle_history_import_body(httpd_req_t *req)
{
    /* content_len у esp_http_server — size_t, поэтому «пусто» это ровно ноль,
       а не «меньше или равно нулю»: со знаковым сравнением компилятор ругается
       на заведомо ложное условие, и проверка выглядела бы работающей. */
    if (req->content_len == 0 || req->content_len > RADON_IMPORT_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "размер тела превышает допустимый");
        return ESP_FAIL;
    }

    if (!radon_stats_import_begin()) {
        /* В esp_http_server кода 503 в httpd_err_code_t нет — ближайший по
           смыслу «сервер занят/не может обслужить» это 500; текст объясняет
           причину. Полосу 503 отдаёт обёртка через http_io_gate. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "импорт уже идёт либо хранилище недоступно");
        return ESP_FAIL;
    }

    static char rbuf[1024];
    static char lbuf[192];
    static size_t lpos = 0;

    lpos = 0; // обнуляем позицию строки перед началом

    /* Без sys/param.h макроса MIN в этом файле нет, а подключать заголовок ради
       одной строки — лишняя зависимость: берём минимум явно. */
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining < sizeof(rbuf) ? remaining : sizeof(rbuf);
        int recv_len = httpd_req_recv(req, rbuf, want);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else if (recv_len <= 0) {
            radon_stats_import_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "обрыв приёма");
            return ESP_FAIL;
        }

        for (int i = 0; i < recv_len; ++i) {
            char c = rbuf[i];
            if (c == '\r') {
                continue; // игнорируем \r
            } else if (c == '\n') {
                lbuf[lpos] = '\0';
                if (!radon_stats_import_line(lbuf)) {
                    radon_stats_import_abort();
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ошибка импорта строки");
                    return ESP_FAIL;
                }
                lpos = 0;
            } else {
                if (lpos < sizeof(lbuf) - 1) {
                    lbuf[lpos++] = c;
                }
            }
        }

        remaining -= (size_t)recv_len;
    }

    // если осталась незавершённая строка
    if (lpos > 0) {
        lbuf[lpos] = '\0';
        if (!radon_stats_import_line(lbuf)) {
            radon_stats_import_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ошибка импорта строки");
            return ESP_FAIL;
        }
    }

    int n = radon_stats_import_commit();
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "импорт отклонён: годных строк нет либо хранилище занято");
        return ESP_FAIL;
    }

    char out[96];
    int len = snprintf(out, sizeof(out), "{\"ok\":true,\"rows\":%d,\"generation\":%u}",
                       n, (unsigned)radon_stats_generation());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, len);
    ESP_LOGW(TAG, "#RADEX-174: импорт истории принят, строк: %d", n);

    return ESP_OK;
}

static esp_err_t handle_history_import(httpd_req_t *req)
{
    // #RADEX-174: импорт переписывает файл истории целиком — это самая тяжёлая
    // файловая операция в прошивке, она обязана идти в той же полосе, что экспорт и график,
    // иначе запись пойдёт одновременно с чтением
    if (!http_io_gate_enter_wait_or_503(req, 5000)) return ESP_OK;
    esp_err_t rc = handle_history_import_body(req);
    http_io_gate_leave();
    return rc;
}

static esp_err_t handle_system(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    // Мощности передатчиков спрашиваем У ЧИПА, а не берём из конфигурации.
    // Причина (W-047, 28.08): уставка в конфиге — намерение, а не факт; трое
    // суток сравнений двух прошивок шли на разной мощности и никто этого не
    // видел, потому что на экране её просто не было. Здесь же снимается
    // и путаница «мощность против уровня приёма»: RSSI ниже — это слышимость
    // роутера, к мощности передатчика платы отношения не имеющая.
    int8_t wifi_q = 0;                       // единицы 0.25 dBm
    esp_wifi_get_max_tx_power(&wifi_q);
    int wifi_dbm = wifi_q / 4;
    // Шкала уровней BLE НЕ линейна на верхнем конце: до P18 шаг 3 дБ, а
    // последняя ступень — P20, не P21. Пересчёт формулой (lvl-N0)*3 давал для
    // неё 21 dBm вместо 20 — проверено на живой плате 28.08. Поэтому таблица.
    static const int8_t BLE_DBM[] = {
        -24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20
    };
    esp_power_level_t lvl = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT);
    int ble_dbm = ((int)lvl >= 0 && (int)lvl < (int)(sizeof(BLE_DBM)/sizeof(BLE_DBM[0])))
                  ? BLE_DBM[(int)lvl] : 0;

    char buf[512];
    /* #RADEX-188: адрес прибора, к которому привязана плата. MAC в эфире может
       принадлежать чужому Radex, и раньше по Web UI это было никак не видно. */
    char dev_mac[18] = "";
    ble_radex_target_mac(dev_mac, sizeof(dev_mac));

    int n = snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"uptime_sec\":%lld,\"free_heap\":%u,\"heap_total\":%u,\"min_free_heap\":%u,"
        "\"psram_free\":%u,\"wifi_rssi\":%d,\"wifi_connected\":%s,\"ap_mode\":%s,"
        /* #RADEX-171/172: наблюдаемость защиты от гонки. io_rejects — сколько
           тяжёлых запросов получили 503 (это НЕ ошибка, полоса работает как
           задумано); lock_timeouts — сколько раз мьютекс истории НЕ достался
           за отведённое время. Второй счётчик обязан быть нулём: ненулевой
           означает, что запись измерения из BLE-задачи не дождалась долгого
           читателя (экспорт большого файла на слабом канале) и показание
           потеряно. Молча такое не должно происходить — потому и в /api/system. */
        "\"wifi_tx_dbm\":%d,\"ble_tx_dbm\":%d,"
        "\"io_rejects\":%u,\"lock_timeouts\":%u,\"dev_mac\":\"%s\"}",
        app ? app->version : "?",
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(),
        /* #RADEX-106: без общего объёма процент посчитать не из чего — страница
           показывала бы долю от выдуманного числа. */
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        rssi,
        wifi_is_connected() ? "true" : "false",
        wifi_manager_is_ap_mode() ? "true" : "false",
        wifi_dbm, ble_dbm,
        (unsigned)http_io_gate_reject_count(),
        (unsigned)radon_stats_lock_timeouts(),
        dev_mac);
    if (n < 0 || n >= (int)sizeof(buf)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handle_health(httpd_req_t *req)
{
    bool ok = wifi_is_connected();
    httpd_resp_set_status(req, ok ? "200 OK" : "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ok ? "ok" : "no-net", HTTPD_RESP_USE_STRLEN);
}


// ── Выбор прибора ─────────────────────────────────────────────────────────
static esp_err_t handle_scan(httpd_req_t *req)
{
    char buf[768];
    int n = ble_radex_found_json(buf, sizeof(buf));
    if (n < 0 || n >= (int)sizeof(buf)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void restart_cb(void *arg) { esp_restart(); }

// POST /api/target, тело: AA:BB:CC:DD:EE:FF
// Перезагрузка отложена: сначала отдаём ответ, иначе браузер получит обрыв
// соединения вместо подтверждения и не поймёт, применилось ли.
/* #RADEX-190, оператор: «нужный прибор не видит». Поиск в эфире идёт ТОЛЬКО пока
   прибор не выбран (ble_radex.c: scan_start вызывается при !s_have_target). Если
   плата успела привязаться к чужому Radex по соседству, свой в списке уже не
   появится — а сброса привязки не было ни в API, ни в UI. Этот эндпоинт его даёт. */
static esp_err_t handle_target_clear(httpd_req_t *req)
{
    ble_radex_clear_target();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_target(httpd_req_t *req)
{
    char body[64] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (len <= 0 || httpd_req_recv(req, body, len) <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    body[len] = 0;

    if (!ble_radex_set_target(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"restart_ms\":1500}", HTTPD_RESP_USE_STRLEN);

    const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
    esp_timer_handle_t th;
    if (esp_timer_create(&a, &th) == ESP_OK) esp_timer_start_once(th, 1500 * 1000);
    return ESP_OK;
}


// ── Home Assistant (MQTT) ────────────────────────────────────────────────
static esp_err_t handle_ha_get(httpd_req_t *req)
{
    char buf[320];
    int n = ha_mqtt_config_json(buf, sizeof(buf));
    if (n < 0 || n >= (int)sizeof(buf)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// POST /api/ha — тело: uri|user|pass (user и pass могут быть пустыми).
// Разделитель '|' выбран потому, что в MQTT-URI он не встречается.
static esp_err_t handle_ha_set(httpd_req_t *req)
{
    char body[224] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (len <= 0 || httpd_req_recv(req, body, len) <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    body[len] = 0;

    char *user = strchr(body, '|');
    char *pass = NULL;
    if (user) { *user++ = 0; pass = strchr(user, '|'); if (pass) *pass++ = 0; }

    if (!ha_mqtt_set_config(body, user, pass))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad config");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"restart_ms\":1500}", HTTPD_RESP_USE_STRLEN);
    const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart_ha" };
    esp_timer_handle_t th;
    if (esp_timer_create(&a, &th) == ESP_OK) esp_timer_start_once(th, 1500 * 1000);
    return ESP_OK;
}

static const httpd_uri_t s_uris[] = {
    { .uri = "/",              .method = HTTP_GET, .handler = handle_root   },
    { .uri = "/api/data",      .method = HTTP_GET, .handler = handle_data   },
    { .uri = "/api/system",    .method = HTTP_GET, .handler = handle_system },
    { .uri = "/api/scan",      .method = HTTP_GET,  .handler = handle_scan   },
    { .uri = "/api/target",    .method = HTTP_POST, .handler = handle_target },
    { .uri = "/api/target/clear", .method = HTTP_POST, .handler = handle_target_clear },
    { .uri = "/api/ha",        .method = HTTP_GET,  .handler = handle_ha_get },
    { .uri = "/api/ha",        .method = HTTP_POST, .handler = handle_ha_set },
    { .uri = "/api/history",   .method = HTTP_GET,  .handler = handle_history },
    { .uri = "/api/log",       .method = HTTP_GET,  .handler = handle_log    },
    { .uri = "/api/log.txt",   .method = HTTP_GET,  .handler = handle_log_download },
    { .uri = "/api/log",       .method = HTTP_DELETE, .handler = handle_log_clear },
    { .uri = "/api/narodmon",  .method = HTTP_GET,  .handler = handle_nm_get },
    { .uri = "/api/narodmon",  .method = HTTP_POST, .handler = handle_nm_set },
    { .uri = "/api/narodmon/enable", .method = HTTP_POST, .handler = handle_nm_enable },
    { .uri = "/api/stats",     .method = HTTP_GET,  .handler = handle_stats },
    { .uri = "/api/stats",     .method = HTTP_POST, .handler = handle_stats_set },
    { .uri = "/api/ntp",       .method = HTTP_GET,  .handler = handle_ntp_get },
    { .uri = "/api/ntp",       .method = HTTP_POST, .handler = handle_ntp_set },
    { .uri = "/api/time",      .method = HTTP_POST, .handler = handle_time_set },
    { .uri = "/api/export.csv",.method = HTTP_GET,  .handler = handle_export },
    { .uri = "/api/test",      .method = HTTP_POST, .handler = handle_test_ctl },
    { .uri = "/api/history/range", .method = HTTP_GET, .handler = handle_history_range },
    { .uri = "/api/history/import", .method = HTTP_POST, .handler = handle_history_import },   /* #RADEX-174 */
    { .uri = "/api/assess", .method = HTTP_GET, .handler = handle_assess },
    { .uri = "/api/cycle", .method = HTTP_GET, .handler = handle_cycle },
    { .uri = "/api/compact", .method = HTTP_POST, .handler = handle_compact },
    { .uri = "/api/reboot",  .method = HTTP_POST, .handler = handle_reboot },   /* #RADEX-148 */
    { .uri = "/api/tests",  .method = HTTP_GET, .handler = handle_tests_list },
    { .uri = "/api/tests/points", .method = HTTP_GET, .handler = handle_tests_points },
    { .uri = "/healthcheck",   .method = HTTP_GET,  .handler = handle_health },
};
#define URI_COUNT (sizeof(s_uris) / sizeof(s_uris[0]))

void web_server_init(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id           = 1;      // ядро 0 занято радио (Wi-Fi + BLE)
    cfg.max_uri_handlers  = URI_COUNT + 8;   // запас под будущие страницы
    cfg.stack_size        = 6144;
    cfg.max_open_sockets  = 7;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t e = httpd_start(&server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start не удался: %s", esp_err_to_name(e));
        return;
    }
    for (size_t i = 0; i < URI_COUNT; i++) {
        e = httpd_register_uri_handler(server, &s_uris[i]);
        if (e != ESP_OK)
            ESP_LOGE(TAG, "URI %s не зарегистрирован: %s", s_uris[i].uri, esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "web-сервер поднят, обработчиков: %u", (unsigned)URI_COUNT);
}
