/*
 * narodmon.c - выгрузка показаний на narodmon.ru (ESP-IDF v5, C)
 *
 * ВНИМАНИЕ: выгрузка по умолчанию выключена и после каждой перезагрузки платы снова выключена.
 * Флаг включения в энергонезависимую память НЕ пишется никогда — только адрес станции и интервал.
 * Признак "включено" живёт в оперативной памяти и теряется при сбросе, отключении питания
 * и обновлении прошивки. Это осознанное требование: включение — всегда явное действие человека,
 * никакая перезагрузка не должна возобновить отправку сама.
 *
 * Автор: [Ваше имя]
 * Лицензия: MIT
 */

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "narodmon.h"
#include "radex_data.h"

static const char *TAG = "narodmon";

// Состояние модуля
static bool g_enabled = false;              // Включено в текущем сеансе
static char g_station_mac[18] = {0};        // Адрес станции в формате AA:BB:CC:DD:EE:FF
static int g_interval_min = 10;             // Интервал отправки в минутах
static int64_t g_last_send_time = 0;        // Время последней успешной отправки (микросекунды)
static char g_last_error[128] = {0};        // Последняя ошибка

// Проверка формата MAC-адреса
static bool is_valid_mac(const char *mac) {
    if (!mac || strlen(mac) != 17) return false;
    
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else {
            if (!((mac[i] >= '0' && mac[i] <= '9') || 
                  (mac[i] >= 'A' && mac[i] <= 'F') || 
                  (mac[i] >= 'a' && mac[i] <= 'f'))) {
                return false;
            }
        }
    }
    
    return true;
}

// Инициализация модуля
void narodmon_start(void) {
    ESP_LOGI(TAG, "Инициализация модуля narodmon");
    
    // Сброс состояния (всегда выключено после перезагрузки)
    g_enabled = false;
    g_last_send_time = 0;
    g_last_error[0] = '\0';
    
    // Читаем настройки из NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("nmon", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Не удалось открыть NVS для чтения: %s", esp_err_to_name(err));
        return;
    }
    
    // Читаем MAC-адрес
    size_t mac_len = sizeof(g_station_mac);
    err = nvs_get_str(nvs_handle, "mac", g_station_mac, &mac_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Не удалось прочитать MAC из NVS: %s", esp_err_to_name(err));
        g_station_mac[0] = '\0';
    } else if (!is_valid_mac(g_station_mac)) {
        ESP_LOGW(TAG, "Некорректный формат MAC-адреса в NVS");
        g_station_mac[0] = '\0';
    }
    
    // Читаем интервал
    int32_t interval;
    err = nvs_get_i32(nvs_handle, "intmin", &interval);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Не удалось прочитать интервал из NVS: %s", esp_err_to_name(err));
        g_interval_min = 10;
    } else {
        if (interval < 5) {
            ESP_LOGW(TAG, "Интервал меньше 5 минут, установлено значение по умолчанию 10");
            g_interval_min = 10;
        } else if (interval > 1440) {
            ESP_LOGW(TAG, "Интервал больше 1440 минут, установлено значение по умолчанию 10");
            g_interval_min = 10;
        } else {
            g_interval_min = interval;
        }
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "narodmon инициализирован: MAC=%s, интервал=%d мин", 
             g_station_mac[0] ? g_station_mac : "(не задан)", g_interval_min);
}

// Установка конфигурации
bool narodmon_set_config(const char *station_mac, int interval_min) {
    // Проверяем формат MAC-адреса
    if (!is_valid_mac(station_mac)) {
        ESP_LOGE(TAG, "Некорректный формат MAC-адреса: %s", station_mac ? station_mac : "(null)");
        return false;
    }
    
    // Проверяем интервал
    if (interval_min < 5) {
        ESP_LOGE(TAG, "Интервал меньше 5 минут: %d", interval_min);
        return false;
    }
    
    if (interval_min > 1440) {
        ESP_LOGE(TAG, "Интервал больше 1440 минут: %d", interval_min);
        return false;
    }
    
    // Сохраняем в NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("nmon", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось открыть NVS для записи: %s", esp_err_to_name(err));
        return false;
    }
    
    // Сохраняем MAC-адрес
    err = nvs_set_str(nvs_handle, "mac", station_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось записать MAC в NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    // Сохраняем интервал
    err = nvs_set_i32(nvs_handle, "intmin", interval_min);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось записать интервал в NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось зафиксировать изменения в NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    
    // Обновляем локальные переменные
    strncpy(g_station_mac, station_mac, sizeof(g_station_mac) - 1);
    g_station_mac[sizeof(g_station_mac) - 1] = '\0';
    g_interval_min = interval_min;
    
    ESP_LOGI(TAG, "Конфигурация сохранена: MAC=%s, интервал=%d мин", 
             g_station_mac, g_interval_min);
    
    return true;
}

// Включение/выключение модуля
bool narodmon_set_enabled(bool on) {
    if (on && !g_station_mac[0]) {
        ESP_LOGW(TAG, "Попытка включения без заданного MAC-адреса");
        return false;
    }
    
    g_enabled = on;
    ESP_LOGI(TAG, "narodmon %s", on ? "включен" : "выключен");
    return true;
}

// Проверка состояния
bool narodmon_is_enabled(void) {
    return g_enabled;
}

// Основная функция отправки данных
void narodmon_publish(void) {
    // Проверяем, включено ли
    if (!g_enabled) {
        ESP_LOGD(TAG, "narodmon выключен");
        return;
    }
    
    // Проверяем, задан ли MAC-адрес
    if (!g_station_mac[0]) {
        ESP_LOGW(TAG, "MAC-адрес не задан");
        return;
    }
    
    // Получаем данные с Radex
    radex_data_t data;
    radex_data_get(&data);   // функция заполняет структуру, а не возвращает её
    if (!data.valid) {
        ESP_LOGW(TAG, "Показания Radex недействительны");
        return;
    }
    
    // Проверяем подключение к Wi-Fi
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Не удалось получить информацию о Wi-Fi: %s", esp_err_to_name(err));
        return;
    }
    
    // Если нет подключения к Wi-Fi
    if (strlen((char*)ap_info.ssid) == 0) {
        ESP_LOGW(TAG, "Нет подключения к Wi-Fi");
        return;
    }
    
    // Проверяем интервал отправки
    int64_t now = esp_timer_get_time();
    int64_t interval_us = (int64_t)g_interval_min * 60 * 1000000; // Интервал в микросекундах
    
    if (g_last_send_time > 0 && (now - g_last_send_time) < interval_us) {
        ESP_LOGD(TAG, "Интервал отправки еще не истек");
        return;
    }
    
    // Формируем строку для отправки
    char send_buffer[256];
    int len = snprintf(send_buffer, sizeof(send_buffer),
                       "#%s#Radex MR107ion\n"
                       "#R1#%.1f\n"
                       "#T1#%.1f\n"
                       "#H1#%.0f\n"
                       "##\n",
                       g_station_mac,
                       data.radon_last,
                       data.temperature,
                       data.humidity);
    
    if (len >= sizeof(send_buffer)) {
        ESP_LOGE(TAG, "Буфер отправки переполнен");
        return;
    }
    
    // Подключаемся к серверу
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    struct addrinfo *result = NULL;
    int status = getaddrinfo("narodmon.ru", "8283", &hints, &result);
    if (status != 0) {
        // gai_strerror в lwIP отдаёт не строку, а код — печатаем числом.
        ESP_LOGE(TAG, "не удалось разрешить имя сервера, код %d", status);
        return;
    }
    
    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Ошибка создания сокета: %s", strerror(errno));
        freeaddrinfo(result);
        return;
    }
    
    // Устанавливаем таймауты
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Ошибка установки таймаута приема: %s", strerror(errno));
        close(sock);
        freeaddrinfo(result);
        return;
    }
    
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Ошибка установки таймаута передачи: %s", strerror(errno));
        close(sock);
        freeaddrinfo(result);
        return;
    }
    
    // Подключаемся к серверу
    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "Ошибка подключения к серверу: %s", strerror(errno));
        close(sock);
        freeaddrinfo(result);
        return;
    }
    
    freeaddrinfo(result);
    
    // Отправляем данные
    ssize_t sent = send(sock, send_buffer, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Ошибка отправки данных: %s", strerror(errno));
        close(sock);
        return;
    }
    
    // Читаем ответ
    char response[128];
    ssize_t received = recv(sock, response, sizeof(response) - 1, 0);
    if (received < 0) {
        ESP_LOGE(TAG, "Ошибка получения ответа: %s", strerror(errno));
        close(sock);
        return;
    }
    
    response[received] = '\0';
    
    // Закрываем сокет
    close(sock);
    
    // Обрабатываем ответ
    if (strstr(response, "OK")) {
        // Успешная отправка
        g_last_send_time = now;
        g_last_error[0] = '\0';
        ESP_LOGI(TAG, "Успешно отправлено на narodmon.ru");
    } else {
        // Ошибка отправки
        strncpy(g_last_error, response, sizeof(g_last_error) - 1);
        g_last_error[sizeof(g_last_error) - 1] = '\0';
        
        ESP_LOGW(TAG, "Ошибка сервера: %s", response);
        
        // Обрабатываем специфические ошибки
        if (strstr(response, "INTERVAL")) {
            ESP_LOGW(TAG, "Отправки чаще разрешённого, увеличьте интервал");
        } else if (strstr(response, "BANNED")) {
            ESP_LOGW(TAG, "Станция заблокирована сервером");
        } else if (strstr(response, "MAC_MISSED")) {
            ESP_LOGW(TAG, "Адрес станции не зарегистрирован на narodmon.ru");
        } else if (strstr(response, "ERROR")) {
            ESP_LOGW(TAG, "Сервер отверг пакет");
        }
    }
}

// Формирование JSON-конфигурации
int narodmon_config_json(char *buf, size_t len) {
    int64_t last_ok_sec = -1;
    if (g_last_send_time > 0) {
        last_ok_sec = (esp_timer_get_time() - g_last_send_time) / 1000000;
    }
    
    return snprintf(buf, len,
                    "{\"mac\":\"%s\",\"interval_min\":%d,\"enabled\":%s,"
                    "\"configured\":%s,\"last_ok_sec\":%lld,\"last_error\":\"%s\"}",
                    g_station_mac[0] ? g_station_mac : "",
                    g_interval_min,
                    g_enabled ? "true" : "false",
                    g_station_mac[0] ? "true" : "false",
                    (long long) last_ok_sec,
                    g_last_error);
}
