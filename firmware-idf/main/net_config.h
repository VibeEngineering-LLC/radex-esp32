// ══════════════════════════════════════════════════════════════════════════
//  net_config.h — сетевые типы и API шлюза.
//
//  Заменяет общий заголовок донора (atomspectra.h), где сетевые прототипы
//  лежали вперемешку с типами спектрометра. Здесь — только сеть, чтобы
//  предметная область донора не тянулась в проект транзитивно.
// ══════════════════════════════════════════════════════════════════════════
#pragma once

#include <stdbool.h>

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

// ── web_server.c ──────────────────────────────────────────────────────────
void web_server_init(void);
