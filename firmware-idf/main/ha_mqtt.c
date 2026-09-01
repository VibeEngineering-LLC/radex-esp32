// ══════════════════════════════════════════════════════════════════════════
//  ha_mqtt.c — публикация показаний в Home Assistant через MQTT Discovery.
//
//  Почему Discovery, а не RESTful-сенсоры в configuration.yaml: сущности
//  заводятся сами и переживают перепрошивку, а правка конфигурации HA — чужая
//  зона (там своя ответственность за файл). Шлюз только публикует.
//
//  Интеграция ОПЦИОНАЛЬНА: пока адрес брокера в NVS не задан, модуль молчит и
//  ничего не тратит. Показания при этом доступны по HTTP как обычно.
// ══════════════════════════════════════════════════════════════════════════
#include "ha_mqtt.h"
#include "radex_data.h"
#include "ble_radex.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ha";

#define HA_NVS_NS   "ha"
#define URI_MAX     96
#define CRED_MAX    48

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static char s_uri[URI_MAX];
static char s_user[CRED_MAX];
static char s_pass[CRED_MAX];
static char s_node[24];        // radex-gw-AABBCC, уникален по MAC платы
static bool s_discovery_sent;

static void node_id_init(void)
{
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(s_node, sizeof(s_node), "radex-gw-%02x%02x%02x", m[3], m[4], m[5]);
}

// Один конфиг-топик на сущность. retain=1: HA подхватит их и после своего
// перезапуска, не дожидаясь нашего.
static void publish_one_discovery(const char *key, const char *name,
                                  const char *unit, const char *dev_class,
                                  const char *value_key)
{
    char topic[128], payload[640];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config", s_node, key);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\","
        "\"uniq_id\":\"%s_%s\","
        "\"stat_t\":\"%s/state\","
        "\"avty_t\":\"%s/status\","
        "\"val_tpl\":\"{{ value_json.%s }}\","
        "%s%s%s"
        "%s%s%s"
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Radex MR107ion\","
        "\"mf\":\"Quarta-Rad\",\"mdl\":\"MR107ion\",\"sw\":\"%s\"}}",
        name, s_node, key, s_node, s_node, value_key,
        unit      ? "\"unit_of_meas\":\"" : "", unit      ? unit      : "", unit      ? "\"," : "",
        dev_class ? "\"dev_cla\":\""      : "", dev_class ? dev_class : "", dev_class ? "\"," : "",
        s_node, "0.5.0");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
}

static void publish_discovery(void)
{
    // Для радона в HA нет подходящего device_class (radon появился не во всех
    // версиях) — оставляем без класса, единицу задаём явно.
    publish_one_discovery("radon",     "Радон",              "Bq/m³", NULL,          "radon_last");
    publish_one_discovery("radon_avg", "Радон, среднее",     "Bq/m³", NULL,          "radon_avg");
    publish_one_discovery("temp",      "Температура",        "°C",    "temperature", "temperature");
    publish_one_discovery("hum",       "Влажность",          "%",     "humidity",    "humidity");
    publish_one_discovery("rssi",      "BLE: сигнал прибора","dBm",   "signal_strength", "rssi");
    ESP_LOGI(TAG, "конфигурации сущностей опубликованы (%s)", s_node);
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t) data;
    char topic[96];
    switch ((esp_mqtt_event_id_t) id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "брокер подключён: %s", s_uri);
            snprintf(topic, sizeof(topic), "%s/status", s_node);
            esp_mqtt_client_publish(e->client, topic, "online", 0, 1, 1);
            if (!s_discovery_sent) { publish_discovery(); s_discovery_sent = true; }
            ha_mqtt_publish();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "брокер отключён");
            break;
        default:
            break;
    }
}

void ha_mqtt_start(void)
{
    nvs_handle_t h;
    if (nvs_open(HA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_uri);  nvs_get_str(h, "uri",  s_uri,  &l);
        l = sizeof(s_user);        nvs_get_str(h, "user", s_user, &l);
        l = sizeof(s_pass);        nvs_get_str(h, "pass", s_pass, &l);
        nvs_close(h);
    }
    if (s_uri[0] == 0) {
        ESP_LOGI(TAG, "брокер не задан — интеграция с Home Assistant выключена");
        return;
    }
    node_id_init();

    char lwt[96];
    snprintf(lwt, sizeof(lwt), "%s/status", s_node);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_pass[0] ? s_pass : NULL,
        // Завещание: если плата пропадёт, HA пометит сущности недоступными,
        // а не оставит навсегда последние значения как актуальные.
        .session.last_will = {
            .topic = lwt, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "не удалось создать MQTT-клиент"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK)
        ESP_LOGE(TAG, "MQTT-клиент не стартовал");
    else
        ESP_LOGI(TAG, "подключаюсь к брокеру %s как %s", s_uri, s_node);
}

void ha_mqtt_publish(void)
{
    if (!s_client || !s_connected) return;
    radex_data_t d;
    radex_data_get(&d);
    if (!d.valid) return;               // нечего публиковать — молчим

    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    char topic[96], payload[256];
    snprintf(topic, sizeof(topic), "%s/state", s_node);
    snprintf(payload, sizeof(payload),
        "{\"radon_last\":%.1f,\"radon_avg\":%.1f,\"temperature\":%.1f,"
        "\"humidity\":%.0f,\"rssi\":%d,\"reads\":%lu}",
        d.radon_last, d.radon_avg, d.temperature, d.humidity, rssi,
        (unsigned long) ble_radex_reads_ok());
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
}

bool ha_mqtt_enabled(void)   { return s_uri[0] != 0; }
bool ha_mqtt_connected(void) { return s_connected; }

bool ha_mqtt_set_config(const char *uri, const char *user, const char *pass)
{
    if (!uri || strlen(uri) >= URI_MAX) return false;
    if (user && strlen(user) >= CRED_MAX) return false;
    if (pass && strlen(pass) >= CRED_MAX) return false;
    nvs_handle_t h;
    if (nvs_open(HA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_set_str(h, "uri",  uri);
    esp_err_t e2 = nvs_set_str(h, "user", user ? user : "");
    esp_err_t e3 = nvs_set_str(h, "pass", pass ? pass : "");
    esp_err_t e4 = nvs_commit(h);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK);
}

// Пароль наружу не отдаём — только признак того, что он задан.
int ha_mqtt_config_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "{\"uri\":\"%s\",\"user\":\"%s\",\"has_pass\":%s,"
        "\"enabled\":%s,\"connected\":%s,\"node_id\":\"%s\"}",
        s_uri, s_user, s_pass[0] ? "true" : "false",
        ha_mqtt_enabled() ? "true" : "false",
        s_connected ? "true" : "false", s_node);
}
