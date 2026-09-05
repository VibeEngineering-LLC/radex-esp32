/*
 * Реализация протокола Improv Serial для ESP-IDF v5 (ESP32-S3)
 * Ссылка на протокол: https://improv-wifi.com/serial
 * UART0 используется совместно с логами, но клиент ищет сигнатуру IMPROV в потоке,
 * поэтому логи не мешают работе.
 */

#include "improv_serial.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "net_config.h"

#define UART_NUM UART_NUM_0
#define FRAME_SIGNATURE {'I','M','P','R','O','V'}
#define FRAME_VERSION 0x01
#define PACKET_TYPE_CURRENT_STATE 0x01
#define PACKET_TYPE_ERROR_STATE 0x02
#define PACKET_TYPE_RPC 0x03
#define PACKET_TYPE_RPC_RESULT 0x04

#define STATE_READY 0x02
#define STATE_PROVISIONING 0x03
#define STATE_PROVISIONED 0x04

#define ERROR_NONE 0x00
#define ERROR_INVALID_RPC 0x01
#define ERROR_UNKNOWN_RPC 0x02
#define ERROR_UNABLE_TO_CONNECT 0x03
#define ERROR_UNKNOWN 0xFF

#define CMD_WIFI_SETTINGS 0x01
#define CMD_IDENTIFY 0x02
#define CMD_GET_CURRENT_STATE 0x03
#define CMD_GET_DEVICE_INFO 0x04
#define CMD_GET_WIFI_NETWORKS 0x05

static const char *TAG = "improv";

/* Кадр и строка лога идут в ОДИН UART из разных задач, и без общего замка лог
   влезает в середину кадра: при первой же проверке на плате ответ PROVISIONED
   вышел как «IMP» + строка лога + «ROV…» и клиентом опознан не был. Замок
   один на оба потока: наш vprintf-перехватчик берёт его на время строки лога,
   improv_send — на время кадра. Внутри improv_send логировать НЕЛЬЗЯ (мьютекс
   не рекурсивный). */
static SemaphoreHandle_t s_tx_mux = NULL;
static vprintf_like_t s_prev_vprintf = NULL;

static int improv_log_vprintf(const char *fmt, va_list args)
{
    if (s_tx_mux == NULL || s_prev_vprintf == NULL) return 0;
    xSemaphoreTake(s_tx_mux, portMAX_DELAY);
    int n = s_prev_vprintf(fmt, args);
    xSemaphoreGive(s_tx_mux);
    return n;
}

static void improv_send(uint8_t type, const uint8_t *data, uint8_t len);
static void improv_send_state(uint8_t state);
static void improv_send_error(uint8_t err);
static void improv_send_rpc_result(uint8_t cmd, const char **strings, int n);
static bool improv_board_url(char *out, size_t out_sz);

/* Кадр копится ЦЕЛИКОМ, вместе с сигнатурой, версией, типом и длиной: контрольная
   сумма по протоколу считается по всем предыдущим байтам, и хранить отдельно одни
   лишь данные означало бы считать её по половине кадра. Данные начинаются с
   HDR_LEN. Ёмкость: 9 байт заголовка + 200 данных + байт суммы. */
#define HDR_LEN 9
#define IMPROV_MAX_DATA 200
static uint8_t s_buf[HDR_LEN + IMPROV_MAX_DATA + 1];
static size_t s_pos = 0;
static uint8_t s_type = 0;
static uint8_t s_len = 0;

static void improv_task(void *arg)
{
    uint8_t byte;
    while (1) {
        if (uart_read_bytes(UART_NUM, &byte, 1, pdMS_TO_TICKS(100)) == 1) {
            s_buf[s_pos] = byte;
            if (s_pos < 6) {
                if (byte == "IMPROV"[s_pos]) {
                    s_pos++;
                } else {
                    /* Сорванная синхронизация: 'I' может быть началом следующей
                       сигнатуры, всё прочее — мусор до неё. */
                    s_pos = 0;
                    if (byte == 'I') {
                        s_buf[0] = byte;
                        s_pos = 1;
                    }
                }
            } else if (s_pos == 6) {
                s_pos = (byte == FRAME_VERSION) ? s_pos + 1 : 0;
            } else if (s_pos == 7) {
                s_type = byte;
                s_pos++;
            } else if (s_pos == 8) {
                s_len = byte;
                s_pos = (s_len > IMPROV_MAX_DATA) ? 0 : s_pos + 1;
            } else {
                s_pos++;
                if (s_pos == (size_t)(HDR_LEN + s_len + 1)) {
                    /* Кадр целиком. Сумма — по всем байтам, КРОМЕ последнего:
                       последний и есть сумма. */
                    uint8_t checksum = 0;
                    for (size_t i = 0; i + 1 < s_pos; i++) {
                        checksum += s_buf[i];
                    }
                    if (checksum != s_buf[s_pos - 1]) {
                        improv_send_error(ERROR_INVALID_RPC);
                    } else if (s_type == PACKET_TYPE_RPC) {
                        uint8_t cmd = s_len >= 2 ? s_buf[HDR_LEN] : 0;
                        uint8_t data_len = s_len >= 2 ? s_buf[HDR_LEN + 1] : 0;
                        if (s_len < 2 || data_len != s_len - 2) {
                            improv_send_error(ERROR_INVALID_RPC);
                        } else {
                            const uint8_t *data = &s_buf[HDR_LEN + 2];
                            switch (cmd) {
                                case CMD_GET_CURRENT_STATE: {
                                    uint8_t state = wifi_is_connected() ? STATE_PROVISIONED : STATE_READY;
                                    improv_send_state(state);
                                    if (state == STATE_PROVISIONED) {
                                        char url[32];
                                        if (improv_board_url(url, sizeof(url))) {
                                            improv_send_rpc_result(cmd, (const char *[]){url}, 1);
                                        }
                                    }
                                    break;
                                }
                                case CMD_GET_DEVICE_INFO: {
                                    const esp_app_desc_t *app_desc = esp_app_get_description();
                                    const char *strings[] = {
                                        "radex-gw-idf",
                                        app_desc->version,
                                        "ESP32-S3",
                                        "Radex Gateway"
                                    };
                                    improv_send_rpc_result(cmd, strings, 4);
                                    break;
                                }
                                case CMD_IDENTIFY: {
                                    ESP_LOGI(TAG, "запрошено опознание платы");
                                    break;
                                }
                                case CMD_GET_WIFI_NETWORKS: {
                                    improv_send_error(ERROR_UNKNOWN_RPC);
                                    break;
                                }
                                case CMD_WIFI_SETTINGS: {
                                    /* Длина имени сети проверяется ДО чтения длины
                                       пароля: иначе испорченный кадр заставил бы
                                       прочитать байт за концом данных. */
                                    uint8_t ssid_len = data_len >= 1 ? data[0] : 0;
                                    const char *ssid = (const char *)&data[1];
                                    bool room = (ssid_len > 0) && (ssid_len <= 32) &&
                                                ((int)ssid_len + 2 <= (int)data_len);
                                    uint8_t pass_len = room ? data[ssid_len + 1] : 0;
                                    const char *pass = (const char *)&data[ssid_len + 2];
                                    if (!room || pass_len > 64 ||
                                        ssid_len + pass_len + 2 != data_len) {
                                        improv_send_error(ERROR_INVALID_RPC);
                                    } else {
                                        char ssid_buf[33], pass_buf[65];
                                        memcpy(ssid_buf, ssid, ssid_len);
                                        ssid_buf[ssid_len] = '\0';
                                        memcpy(pass_buf, pass, pass_len);
                                        pass_buf[pass_len] = '\0';
                                        improv_send_state(STATE_PROVISIONING);
                                        ESP_LOGI(TAG, "Improv: пробую сеть «%s»", ssid_buf);
                                        char ip[16] = {0};
                                        bool success = wifi_manager_provision(ssid_buf, pass_buf, ip, sizeof(ip), 20000);
                                        if (success) {
                                            /* Клиент ждёт АДРЕС СТРАНИЦЫ, а не голый IP:
                                               по нему он предлагает «открыть устройство». */
                                            char url[40];
                                            snprintf(url, sizeof(url), "http://%s/", ip);
                                            improv_send_state(STATE_PROVISIONED);
                                            improv_send_rpc_result(cmd, (const char *[]){url}, 1);
                                        } else {
                                            improv_send_error(ERROR_UNABLE_TO_CONNECT);
                                            improv_send_state(STATE_READY);
                                        }
                                    }
                                    break;
                                }
                                default: {
                                    improv_send_error(ERROR_UNKNOWN_RPC);
                                    break;
                                }
                            }
                        }
                    }
                    s_pos = 0;
                }
            }
        }
    }
}

static void improv_send(uint8_t type, const uint8_t *data, uint8_t len)
{
    static uint8_t frame[256 + 6 + 1];
    const uint8_t *signature = (const uint8_t *)"IMPROV";
    memcpy(frame, signature, 6);
    frame[6] = FRAME_VERSION;
    frame[7] = type;
    frame[8] = len;
    memcpy(&frame[9], data, len);
    uint8_t checksum = 0;
    for (int i = 0; i < 9 + len; i++) {
        checksum += frame[i];
    }
    frame[9 + len] = checksum;
    if (s_tx_mux != NULL) xSemaphoreTake(s_tx_mux, portMAX_DELAY);
    uart_write_bytes(UART_NUM, (char *)frame, 10 + len);
    uart_write_bytes(UART_NUM, "\n", 1);
    /* Ждём ухода последнего байта ДО снятия замка: иначе следующая строка лога
       обгонит хвост кадра в аппаратной очереди. */
    uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(200));
    if (s_tx_mux != NULL) xSemaphoreGive(s_tx_mux);
}

static void improv_send_state(uint8_t state)
{
    improv_send(PACKET_TYPE_CURRENT_STATE, &state, 1);
}

static void improv_send_error(uint8_t err)
{
    improv_send(PACKET_TYPE_ERROR_STATE, &err, 1);
}

static void improv_send_rpc_result(uint8_t cmd, const char **strings, int n)
{
    uint8_t total_len = 0;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(strings[i]);
        if (len > 64) len = 64;
        total_len += 1 + len;
    }
    static uint8_t buf[256];
    buf[0] = cmd;
    buf[1] = total_len;
    uint8_t pos = 2;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(strings[i]);
        if (len > 64) len = 64;
        buf[pos++] = len;
        memcpy(&buf[pos], strings[i], len);
        pos += len;
    }
    improv_send(PACKET_TYPE_RPC_RESULT, buf, pos);
}

static bool improv_board_url(char *out, size_t out_sz)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }
    snprintf(out, out_sz, "http://" IPSTR "/", IP2STR(&ip_info.ip));
    return true;
}

void improv_serial_start(void)
{
    esp_err_t err = uart_driver_install(UART_NUM, 512, 0, 0, NULL, 0);
    if (err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    s_tx_mux = xSemaphoreCreateMutex();
    if (s_tx_mux != NULL) {
        s_prev_vprintf = esp_log_set_vprintf(improv_log_vprintf);
    } else {
        ESP_LOGE(TAG, "нет памяти под замок вывода — кадры могут рваться логом");
    }
    xTaskCreate(improv_task, "improv", 4096, NULL, 5, NULL);
}
