// ══════════════════════════════════════════════════════════════════════════
//  main.c — точка входа шлюза Radex MR107ion → Wi-Fi.
//
//  Порядок инициализации важен: NVS нужен и Wi-Fi, и BLE; Wi-Fi поднимается
//  до web-сервера; BLE запускается последним, потому что дальше он живёт
//  своей задачей и переподключается сам.
// ══════════════════════════════════════════════════════════════════════════
#include "net_config.h"
#include "ble_radex.h"
#include "radex_data.h"
#include "ha_mqtt.h"
#include "log_ring.h"
#include "narodmon.h"
#include "radon_stats.h"
#include "net_time.h"    /* #RADEX-113: net_time_mark_sntp() */
#include "poll_cycle.h"
#include "http_io_gate.h"   /* #RADEX-171 */

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_system.h>   /* #RADEX-145: esp_reset_reason() */
#include <nvs_flash.h>
#include <nvs.h>

// Своя сеть — чтобы после каждой перепрошивки не проходить captive-портал
// заново. Режим ЛОКАЛЬНЫЙ и включается ТОЛЬКО явно: `.\build.ps1 -LocalWifi`
// (переменная RADEX_GW_LOCAL_WIFI=1). Дефолт сборки — без сетевых данных.
//
// Раньше условием было одно лишь `__has_include`. Файл secrets_local.h лежит
// у разработчика всегда, поэтому «обычная» сборка молча уносила в бинарник
// реальные SSID и пароль его сети — и такие образы ушли в релиз v1.0.0.
// Наличие файла на диске не выражает намерения его опубликовать; намерение
// выражается только явным флагом. Настройка сети принадлежит владельцу платы
// и вносится порталом RadexGW-Setup либо через Web UI.
#if defined(RADEX_GW_LOCAL_WIFI) && __has_include("secrets_local.h")
#  include "secrets_local.h"
// Маркер в .rodata: делает локальный образ узнаваемым сам по себе, без списка
// секретов, — проверяющему не нужно знать, какая именно сеть вшита.
__attribute__((used))
static const char radex_gw_local_build_marker[] =
    "RADEX-GW-LOCAL-BUILD-DO-NOT-PUBLISH";
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "main";

// Точка в историю ставится по СРЕДНЕМУ РАДОНУ — он читается вторым и всегда.
// Привязка к реальному опросу, а не к таймеру: иначе на графике появятся
// «измерения», которых не было (повторённое старое значение выглядит как новое).
// История правок этой привязки:
//   1) радон последний — он читается ПЕРВЫМ, и в точку попадали нули вместо
//      температуры и влажности (поймано на плате 28.08);
//   2) влажность — она была последней в круге, но после введения ротации
//      характеристик (#RADEX-18: прибор отдаёт лишь 4 значения за сессию)
//      приходит через круг, и точки перестали ставиться вовсе — график опустел;
//   3) среднее радона — читается вторым в КАЖДОМ круге. Температура и влажность
//      берутся последние известные; они меняются медленно, и их «возраст»
//      в пределах десятков минут для графика несуществен.
#define H_RADON_AVG_PT 0x0040

static void on_measurement(uint16_t handle, float value)
{
    radex_data_put(handle, value);
    if (handle == H_RADON_AVG_PT) {
        radex_data_t d;
        radex_data_get(&d);
        if (d.valid) {
            /* #RADEX-19: прибор пересчитывает радон циклами 10/20/30/60 мин, а мы
               опрашиваем чаще. Повтор в историю не пишем — иначе одно измерение
               вошло бы в среднее несколько раз и перекосило бы его. */
            if (!poll_cycle_observe(time(NULL), d.radon_last)) return;
            log_ring_hist_push(d.radon_last, d.temperature, d.humidity);
            // На флеш — тем же событием (#RADEX-11). Точка с несинхронизированным
            // временем внутри модуля отбрасывается: неверная метка отравила бы
            // расчёты средних за периоды.
            radon_stats_add(time(NULL), d.radon_last, d.radon_avg,
                            d.temperature, d.humidity);
        }
    }
}

// Записывает сеть в NVS, только если она там ещё не задана: настройки,
// сделанные через портал или Web UI, важнее зашитых и не перетираются.
static void wifi_provision_if_empty(void)
{
#if defined(RADEX_GW_WIFI_SSID)
    // Маркер печатается ЖИВЫМ кодом намеренно: __attribute__((used)) запрещает
    // выбросить символ компилятору, но не линковщику (--gc-sections). Проверено
    // 02.09.2026 — без этой строки маркера в образе не оказалось.
    ESP_LOGW(TAG, "%s", radex_gw_local_build_marker);
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_str(h, "ssid", NULL, &len) == ESP_OK && len > 1) {
        nvs_close(h);
        return;                       // сеть уже настроена — не трогаем
    }
    // Ошибки записи проверяем: молча «настроенная» плата, которая на деле
    // не сохранила сеть, уходит в captive-портал без объяснения причины
    // (находка петли самоаудита 2026-08-27).
    esp_err_t e1 = nvs_set_str(h, "ssid", RADEX_GW_WIFI_SSID);
    esp_err_t e2 = nvs_set_str(h, "pass", RADEX_GW_WIFI_PASS);
    esp_err_t e3 = (e1 == ESP_OK && e2 == ESP_OK) ? nvs_commit(h) : ESP_FAIL;
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        ESP_LOGE(TAG, "не удалось записать сеть в NVS (ssid=%s pass=%s commit=%s)",
                 esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
        return;
    }
    ESP_LOGI(TAG, "сеть по умолчанию записана в NVS: %s", RADEX_GW_WIFI_SSID);
#endif
}

/* #RADEX-113: сервер и часовой пояс берутся из настроек, а не зашиты. Причины
   две. Прибор календарного времени не отдаёт вовсе (проверено по карте
   характеристик: t_izm_last — это счётчик секунд от включения), значит метки
   истории целиком держатся на плате. И вторая: без часового пояса плата живёт
   в UTC, а история и заключение читаются человеком по местному времени —
   расхождение в несколько часов выглядит как сбой прибора. */
static char s_ntp_host[64] = "pool.ntp.org";
static char s_tz[48]       = "MSK-3";

void net_time_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("radon", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_ntp_host);
    nvs_get_str(h, "ntp_host", s_ntp_host, &n);
    n = sizeof(s_tz);
    nvs_get_str(h, "tz", s_tz, &n);
    nvs_close(h);
}

const char *net_time_ntp_host(void) { return s_ntp_host; }
const char *net_time_tz(void)       { return s_tz; }

/* #RADEX-113: момент, когда время впервые стало известно. Здесь и только здесь
   история переводится из относительной шкалы в реальную — до этого записи копятся
   с отрицательными метками и НЕ теряются. Обратного вызова раньше не было вовсе,
   поэтому состояние «синхронизировано» не выставлялось никогда и индикатор на
   странице показывал бы «нет» при исправном NTP. */
static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    net_time_mark_sntp();
    time_t now = time(NULL);
    int n = radon_stats_rebase(now);
    if (n > 0) ESP_LOGI(TAG, "время получено по NTP, история переведена: %d записей", n);
    else       ESP_LOGI(TAG, "время получено по NTP");
}

static void init_sntp(void)
{
    net_time_load_config();
    /* Пояс применяем ДО запуска SNTP: localtime() в других задачах может
       сработать раньше первой синхронизации, и без TZ он вернёт UTC. */
    setenv("TZ", s_tz, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_host);
    cfg.sync_cb = on_time_synced;
    esp_netif_sntp_init(&cfg);
}

// #RADEX-145: причина последнего старта — чтобы отличить SW-рестарт
// (esp_restart/watchdog) от POWERON_RESET (питание снималось физически)
// и от рестарта после прошивки (esptool дёргает EN — тоже не POWERON).
static void log_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "причина старта: esp_reset_reason=%d (1=POWERON, 3=SW, "
                  "12=SW_CPU, 14=RTC_SW_CPU, 15=RTCWDT_RTC_RESET)", (int) r);
}

#ifdef RADEX172_STRESS_WRITER
/* #SA-3: вторая ПИШУЩАЯ ЗАДАЧА. Штатный писатель — колбэк BLE — пишет примерно
   раз в час, за три минуты прогона он не столкнётся с читателем ни разу, и
   стенд остаётся зелёным даже со снятым мьютексом (проверено 01.09: мутация
   NO_LOCK дала PASS). Причина: /api/compact исполняется ТОЙ ЖЕ задачей httpd,
   что и читатели, — сервер сериализует их сам, мимо мьютекса. Настоящую гонку
   создаёт только отдельная задача.

   ⚠ Стенд пишет в ОТДЕЛЬНЫЙ файл (radon_stats.c переводит `filename` на
   stress.csv), но 01.09.2026 этого оказалось НЕ достаточно: уплотнение
   игнорировало `filename` и держало путь зашитым, поэтому «безопасный»
   /api/compact во время прогона пересобрал НАСТОЯЩУЮ историю оператора
   (см. #RADEX-173 в radon_stats.c). Дефект исправлен, но урок записан здесь:
   прежде чем считать стенд изолированным, убедиться ГРЕПОМ, что каждый путь
   к файлу в модуле спрашивает `filename`, а не повторяет строку. */
static void radex172_stress_task(void *arg)
{
    (void)arg;
    unsigned i = 0;
    while (1) {
        radon_stats_add(time(NULL), 100.0f + (i % 50), 110.0f, 25.0f, 50.0f);
        i++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Перехват лога в память — ДО остальных подсистем, иначе их собственные
    // сообщения о запуске в браузере не увидеть. Ровно этой слепоты стоила
    // диагностика 27.08: причину отказа приходилось искать по UART.
    log_ring_init();
    log_reset_reason();   /* #RADEX-145: после log_ring_init — видна в /api/log */
    radex_data_init();
    // История на флеш (#RADEX-11). Не смонтировалось — плата продолжает работать
    // без хранилища: показания важнее истории, ронять из-за неё сбор данных нельзя.
    if (!radon_stats_init()) {
        ESP_LOGW(TAG, "хранилище истории недоступно — показания не сохраняются");
    }
    /* #RADEX-44: poll_cycle_init ОБЯЗАН идти ПОСЛЕ монтирования хранилища —
       он читает последнее записанное значение, чтобы после перезагрузки не
       принять повтор за новое измерение. Стоял раньше, файловой системы ещё
       не было, чтение всегда возвращало «нет данных», и защита от дублей не
       работала ни разу: в файле лежат пять записей одного значения 133.67
       подряд (02:01-02:34 29.08), каждая из которых входит в среднее. */
    poll_cycle_init();
    wifi_provision_if_empty();
    wifi_manager_init();
    init_sntp();

    // В режиме первичной настройки порт 80 уже занят captive-порталом самого
    // wifi_manager — свой сервер там поднимать нельзя (первый запуск дал
    // httpd_server_init: error in listen (112), лог
    // logs/hci/radex_gw_idf_first_boot_20260827.log).
    if (wifi_manager_mode() != NET_MODE_SETUP) {
        /* #RADEX-171: семафор тяжёлой полосы — ДО регистрации обработчиков.
           У http_io_gate есть и ленивое создание, но оно само гонка: два
           первых запроса вошли бы в него одновременно. */
        http_io_gate_init();
        web_server_init();
    } else {
        ESP_LOGW(TAG, "режим первичной настройки: показания по HTTP недоступны, "
                      "подключитесь к точке RadexGW-Setup и задайте сеть");
    }

#ifdef RADEX172_STRESS_WRITER
    /* Ядро 0: у httpd закреплено ядро 1 (cfg.core_id), нужна ДРУГАЯ задача на
       другом ядре — иначе снова получим сериализацию вместо гонки. */
    xTaskCreatePinnedToCore(radex172_stress_task, "rs172", 4096, NULL, 4, NULL, 0);
    ESP_LOGW(TAG, "#SA-3: ВКЛЮЧЕН стресс-писатель, файл /data/stress.csv");
#endif

    ha_mqtt_start();
    // Народмон: читает свои настройки, но отправку НЕ включает — она выключена
    // после каждой перезагрузки намеренно (см. шапку narodmon.h).
    narodmon_start();

    ESP_LOGI(TAG, "запускаю BLE-клиент к прибору");
    ble_radex_start(on_measurement);

    // Публикация в Home Assistant раз в 30 с. Реже, чем читаем прибор: HA не
    // нужен каждый опрос, а лишний трафик отнимает эфир у BLE.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ha_mqtt_publish();
        // Свой срок отправки Народмон считает сам (не чаще заданного интервала,
        // минимум 5 мин) и при выключенном состоянии молча выходит.
        narodmon_publish();
    }
}
