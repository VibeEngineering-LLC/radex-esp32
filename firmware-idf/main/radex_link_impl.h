// #USB-1 универсальная сборка: внутренние функции двух реализаций канала связи с прибором —
// BLE (bleimpl_*, ble_radex.c + ble_radex_journal.c) и USB (usbimpl_*, radex_usb.c). Имена им даёт
// -include link_rename_ble.h / link_rename_usb.h; этот заголовок попадает и в сами модули, поэтому
// расхождение объявления с определением — ошибка компиляции, а не тихая порча стека.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ble_radex.h"
#include "radex_journal_parse.h"

#define RADEX_LINK_IMPL_DECL(p)                                  \
    void     p##_start(ble_radex_cb_t cb);                       \
    bool     p##_has_target(void);                               \
    bool     p##_scanning(void);                                 \
    int      p##_found_json(char *buf, size_t len);              \
    bool     p##_set_target(const char *mac_str);                \
    bool     p##_target_mac(char *buf, size_t len);              \
    void     p##_clear_target(void);                             \
    bool     p##_connected(void);                                \
    uint32_t p##_reads_ok(void);                                 \
    uint32_t p##_read_errors(void);                              \
    uint32_t p##_disconnects(void);                              \
    uint32_t p##_open_fails(void);                               \
    void     p##_journal_init(void);                             \
    void     p##_journal_request(void);                          \
    bool     p##_journal_busy(void);                             \
    bool     p##_journal_pending(void);                          \
    int      p##_journal_json(char *buf, size_t len);            \
    bool     p##_journal_get_result(radex_journal_t *out);

RADEX_LINK_IMPL_DECL(bleimpl)
RADEX_LINK_IMPL_DECL(usbimpl)
