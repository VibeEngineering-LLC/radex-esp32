// #USB-1 универсальная сборка: подключается к radex_usb.c через -include (main/CMakeLists.txt).
// Внешние имена USB-модуля получают внутренние usbimpl_*, а сами ble_radex_* реализует диспетчер
// radex_link.c. radex_usb.c от этого не правится.
#pragma once
#define ble_radex_start              usbimpl_start
#define ble_radex_has_target         usbimpl_has_target
#define ble_radex_scanning           usbimpl_scanning
#define ble_radex_found_json         usbimpl_found_json
#define ble_radex_set_target         usbimpl_set_target
#define ble_radex_target_mac         usbimpl_target_mac
#define ble_radex_clear_target       usbimpl_clear_target
#define ble_radex_connected          usbimpl_connected
#define ble_radex_reads_ok           usbimpl_reads_ok
#define ble_radex_read_errors        usbimpl_read_errors
#define ble_radex_disconnects        usbimpl_disconnects
#define ble_radex_open_fails         usbimpl_open_fails
#define ble_radex_journal_init       usbimpl_journal_init
#define ble_radex_journal_request    usbimpl_journal_request
#define ble_radex_journal_busy       usbimpl_journal_busy
#define ble_radex_journal_pending    usbimpl_journal_pending
#define ble_radex_journal_json       usbimpl_journal_json
#define ble_radex_journal_get_result usbimpl_journal_get_result
#include "radex_link_impl.h"   // компилятор сверяет объявления usbimpl_* с определениями модуля
