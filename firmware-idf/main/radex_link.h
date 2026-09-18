// #USB-1 универсальная сборка: канал связи с прибором (реализация — radex_link.c).
#pragma once
#include <stdbool.h>

typedef enum { RADEX_TRANSPORT_NONE = 0, RADEX_TRANSPORT_BLE, RADEX_TRANSPORT_USB } radex_transport_t;

radex_transport_t radex_link_transport(void);
const char *radex_link_transport_str(void);   // "usb" | "ble" | "none" — поле transport в /api/data
bool radex_link_bt_enabled(void);             // настройка «Bluetooth» (NVS radex/bt_on, по умолчанию вкл.)
bool radex_link_set_bt_enabled(bool on);      // запись в NVS, применяется за 0,5 с; false — не записано
bool radex_link_ble_stack_up(void);           // BLE-стек поднят — можно спрашивать мощность передатчика
