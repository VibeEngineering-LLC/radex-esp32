// #USB-1 универсальная сборка: подключается к ble_radex.c и ble_radex_journal.c через -include
// (main/CMakeLists.txt). Внешние имена BLE-модуля получают внутренние bleimpl_*, а сами ble_radex_*
// реализует диспетчер radex_link.c. BLE-файлы от этого не правятся.
#pragma once
#define ble_radex_start              bleimpl_start
#define ble_radex_has_target         bleimpl_has_target
#define ble_radex_scanning           bleimpl_scanning
#define ble_radex_found_json         bleimpl_found_json
#define ble_radex_set_target         bleimpl_set_target
#define ble_radex_target_mac         bleimpl_target_mac
#define ble_radex_clear_target       bleimpl_clear_target
#define ble_radex_connected          bleimpl_connected
#define ble_radex_reads_ok           bleimpl_reads_ok
#define ble_radex_read_errors        bleimpl_read_errors
#define ble_radex_disconnects        bleimpl_disconnects
#define ble_radex_open_fails         bleimpl_open_fails
#define ble_radex_journal_init       bleimpl_journal_init
#define ble_radex_journal_request    bleimpl_journal_request
#define ble_radex_journal_busy       bleimpl_journal_busy
#define ble_radex_journal_pending    bleimpl_journal_pending
#define ble_radex_journal_json       bleimpl_journal_json
#define ble_radex_journal_get_result bleimpl_journal_get_result
#include "radex_link_impl.h"   // компилятор сверяет объявления bleimpl_* с определениями модуля
