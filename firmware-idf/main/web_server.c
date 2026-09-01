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
    static char buf[10240];          // static: 10 КБ на стеке httpd не поместятся
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
    static char buf[8192];
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
    static char buf[8192];
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

    static char buf[1536];   /* static: 1.5 КБ на стеке httpd не помещаются */
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

    static char buf[512];
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
    static char buf[192];
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

static esp_err_t handle_export(httpd_req_t *req) {
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

    static char chunk[1024]; /* тот же довод: стек задачи веб-сервера мал */
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


static esp_err_t handle_history_range(httpd_req_t *req)
{
    // Буфер для строки из файла (128 байт)
    static char line[128];
    
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
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    // Первый проход: подсчитываем количество строк в интервале
    int total = 0;
    long long ts;
    float radon, radon_avg, temp, hum;
    
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lld,%f,%f,%f,%f", &ts, &radon, &radon_avg, &temp, &hum) == 5) {
            // Пропускаем строку заголовка
            if (ts == 0 && radon == 0 && radon_avg == 0 && temp == 0 && hum == 0)
                continue;
                
            // Проверяем попадание в интервал
            if (ts >= from && (to == 0 || ts <= to)) {
                total++;
            }
        }
    }
    
    // Закрываем файл после первого прохода
    fclose(f);
    
    // Если нет подходящих строк, возвращаем пустой ответ
    if (total == 0) {
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    // Вычисляем шаг прореживания
    int step = 1;
    if (total > max) {
        step = total / max;
        if (step < 1) step = 1;
    }
    
    // Открываем файл снова для второго прохода
    f = fopen(radon_stats_file(), "r");
    if (!f) {
        static char empty_resp[] = "{\"n\":0,\"total\":0,\"step\":1,\"points\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, empty_resp, strlen(empty_resp));
    }
    
    // Буфер для отправки частей ответа
    static char chunk[1024];
    int chunk_len = 0;
    
    // Начинаем формировать JSON-ответ
    chunk_len = snprintf(chunk, sizeof(chunk), 
        "{\"total\":%d,\"step\":%d,\"points\":[", total, step);
    
    // Отправляем начальную часть ответа
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, chunk, chunk_len);
    
    int count = 0;
    int sent = 0;   /* сколько точек реально отдано — это и есть n */
    int send_first = 1;  // Флаг для отправки первой точки
    
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lld,%f,%f,%f,%f", &ts, &radon, &radon_avg, &temp, &hum) == 5) {
            // Пропускаем строку заголовка
            if (ts == 0 && radon == 0 && radon_avg == 0 && temp == 0 && hum == 0)
                continue;
                
            // Проверяем попадание в интервал
            if (ts >= from && (to == 0 || ts <= to)) {
                // Если нужно прореживать и шаг не подошел, пропускаем
                if (total > max && count % step != 0 && count != 0 && count != total - 1) {
                    count++;
                    continue;
                }
                
                // Отправляем первую точку всегда
                if (count == 0 || send_first) {
                    send_first = 0;
                } else if (total > max && count % step != 0) {
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
                int point_len = snprintf(chunk, sizeof(chunk),
                    "{\"t\":%lld,\"r\":%s,\"c\":%s,\"h\":%s}", ts, rs, cs, hs);
                
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
static esp_err_t handle_tests_points(httpd_req_t *req)
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
    int n = snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"uptime_sec\":%lld,\"free_heap\":%u,\"heap_total\":%u,\"min_free_heap\":%u,"
        "\"psram_free\":%u,\"wifi_rssi\":%d,\"wifi_connected\":%s,\"ap_mode\":%s,"
        "\"wifi_tx_dbm\":%d,\"ble_tx_dbm\":%d}",
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
        wifi_dbm, ble_dbm);
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
