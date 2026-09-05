// ══════════════════════════════════════════════════════════════════════════
//  net_config.h — сетевые типы и API шлюза.
//
//  Заменяет общий заголовок донора (atomspectra.h), где сетевые прототипы
//  лежали вперемешку с типами спектрометра. Здесь — только сеть, чтобы
//  предметная область донора не тянулась в проект транзитивно.
// ══════════════════════════════════════════════════════════════════════════
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* size_t в сигнатуре wifi_manager_provision (#RADEX-186) */

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

typedef enum {
    NET_MODE_STA = 0,   // клиент домашней сети
    NET_MODE_FIELD_AP,  // собственная точка доступа
    NET_MODE_SETUP,     // captive-портал первичной настройки
} net_run_mode_t;

// ── wifi_manager.c ────────────────────────────────────────────────────────
void            wifi_manager_init(void);
bool            wifi_is_connected(void);
net_run_mode_t  wifi_manager_mode(void);
bool            wifi_manager_is_ap_mode(void);
int             wifi_manager_ap_clients(void);
const char     *wifi_manager_ap_ssid(void);
bool            wifi_manager_ap_pass_is_default(void);
bool            wifi_manager_ap_forced(void);

/* #RADEX-186 шаг Б: применить сеть, пришедшую по Improv Serial. Синхронно ждёт
   подключения до timeout_ms и при успехе пишет адрес в ip_out. Креды
   сохраняются в NVS ТОЛЬКО при успехе: записать непроверенную пару значило бы
   после ближайшей перезагрузки увести плату в сеть, которой нет. */
bool            wifi_manager_provision(const char *ssid, const char *pass,
                                       char *ip_out, size_t ip_sz,
                                       uint32_t timeout_ms);

// ── web_server.c ──────────────────────────────────────────────────────────
void web_server_init(void);
