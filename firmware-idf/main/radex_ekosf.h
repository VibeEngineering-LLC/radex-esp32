// Radex MR107ion по USB (CDC-ACM, VID:PID ABBA:A204): кадр EKOSF + RPC. Чистый модуль без ESP-IDF —
// собирается и тестируется на хосте (test/host/test_radex_ekosf.c).
// Источник протокола: ESP32/STATUS.md раздел #USB-1; эталон ПК scripts/radex_usb/radex_ekosf.py
// (проверен на живом приборе 2026-09-18); байты сверены с трассами captures/radex_usb_0{1,2,3}_*.pcap.
// БЕЗОПАСНОСТЬ: построить можно ТОЛЬКО шесть запросов ниже; radex_req_allowed() — последний гейт
// перед передачей. Иных RPC (в т.ч. 8 FACTORY_RESET, 9 SHUTDOWN, 10 ENTER_BOOT, 14/15 DELETE,
// 23-25 FW_UPLOAD) модуль не строит и не пропускает.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RADEX_USB_VID   0xABBA
#define RADEX_USB_PID   0xA204
#define RADEX_USB_BAUD  230400

#define EKOSF_FT_MASTER 0x7B     // запрос
#define EKOSF_FT_SLAVE  0x7A     // ответ
#define EKOSF_ADDR      0xFF     // адрес прибора — как у RadexDC в трассах
#define EKOSF_HDR_LEN   12
#define EKOSF_CMD_RPC   0x0020
#define EKOSF_RSP_RPC   0x8020

#define RADEX_RPC_SET_TIME    0x0006   // запись часов, u64 мс от 2000-01-01 (местное)
#define RADEX_RPC_CURRENT     0x0BC2   // текущие показания
#define RADEX_RPC_ARCH_BEGIN  0x0C04   // архив, шаг 1 (смысл параметров не вскрыт)
#define RADEX_RPC_ARCH_SEEK   0x0C05   // архив, шаг 3 (смысл параметров не вскрыт)
#define RADEX_RPC_ARCH_HDR    0x0C0D   // архив, шаг 2: заголовок
#define RADEX_RPC_ARCH_PAGE   0x0C0E   // архив, шаг 4: страница 22 записи

#define RADEX_SIG_READ  12
#define RADEX_SIG_ARCH  14
#define RADEX_SIG_TIME  17

#define EKOSF_TX_MAX    32       // длиннейший запрос: 12 + 4 + 8 + 2 = 26 байт

// checksum16 RFC1071: сумма 16-бит слов LE (нечётный хвост — младшим байтом), свёртка, инверсия.
uint16_t ekosf_checksum16(const uint8_t *p, size_t n);

// Построители запросов. Возврат — длина кадра, 0 если cap мал.
size_t radex_req_current(uint8_t *out, size_t cap, uint16_t pnum);
size_t radex_req_arch_begin(uint8_t *out, size_t cap, uint16_t pnum, uint8_t session);
size_t radex_req_arch_hdr(uint8_t *out, size_t cap, uint16_t pnum);
size_t radex_req_arch_seek(uint8_t *out, size_t cap, uint16_t pnum, uint16_t last_idx);
size_t radex_req_arch_page(uint8_t *out, size_t cap, uint16_t pnum);
size_t radex_req_set_time(uint8_t *out, size_t cap, uint16_t pnum, uint64_t ms2000);

// Гейт перед передачей: true только для кадра одного из шести запросов выше
// (заголовок, CRC, rpcId, rpcSig и форма параметров сверяются полностью).
bool radex_req_allowed(const uint8_t *f, size_t n);

// Разбор ответа RPC. res/res_len — результат после [rpcId u16][u16]; frame_len — длина кадра.
#define EKOSF_RSP_NEED_MORE  0
#define EKOSF_RSP_OK         1
#define EKOSF_RSP_BAD_HDR   -1   // не 0x7A или CRC заголовка
#define EKOSF_RSP_BAD_CRC   -2   // CRC данных
#define EKOSF_RSP_MISMATCH  -3   // не тот cmd / packetNum / rpcId
#define EKOSF_RSP_DEV_ERROR -4   // прибор ответил кодом ошибки (cmd >= 0xFF00)
int ekosf_parse_rsp(const uint8_t *buf, size_t len, uint16_t pnum, uint16_t rpc_id,
                    const uint8_t **res, size_t *res_len, size_t *frame_len);

// Текущие показания (0x0BC2). Результат: u32 длина блока (0x58), затем 88 байт.
#define RADEX_CUR_BLOCK_LEN 88
typedef struct {
    // Сверено с трассами 02 и 04 (две точки) и архивом: @12 = ОА последней записи, @20 = её скользящее среднее.
    float    avg;        // @0  средняя ОА текущей сессии прибора, Бк/м3 (= avg сессии в 0x0C0D)
    float    sko;        // @4  СКО средней (= sko сессии в 0x0C0D)
    uint32_t counter;    // @8  счётчик опросов
    float    last_oa;    // @12 ОА последнего цикла (сырая), Бк/м3
    float    cur;        // @20 скользящее среднее (его показывает экран прибора и RadexDC), Бк/м3
    float    temp_c;     // @64 int16 /10, °C
    float    rh;         // @68 u16, %
    uint16_t sessions;   // @74 число сессий в архиве — гипотеза (трассы 02/04: 1 и 2)
    uint16_t session;    // @76 индекс текущей сессии — гипотеза (0 и 1 = параметр 0x0C04 у RadexDC)
    uint8_t  ss, mm, hh, dd, mo, yy;   // @80..85 часы прибора (местное), год = 2000 + yy
    bool     clock_ok;   // поля часов в допустимых пределах; иначе часы прибора не используются,
                         // но показания принимаются (не выставленные часы не должны глушить опрос)
} radex_current_t;
bool radex_parse_current(const uint8_t *res, size_t n, radex_current_t *out);

// Заголовок архива (0x0C0D), трассы 02 (1 сессия) и 04 (2 сессии): u32 длина, u16 сквозной индекс
// последней записи (от 0, параметр 0x0C05), u16 индекс текущей сессии, затем сессии от новой к старой
// по 18 байт; у самой старой prev_gidx = 0xFFFF.
#define RADEX_ARCH_SESS_LEN  18
#define RADEX_ARCH_MAX_SESS  16
typedef struct {
    uint16_t count;         // +0  записей в сессии (номера 1..count)
    uint32_t end_s2000;     // +2  время последней записи сессии, с от 2000-01-01 (местное)
    float    avg;           // +6  средняя ОА сессии
    float    sko;           // +10 СКО средней
    uint16_t prev_gidx;     // +14 сквозной индекс последней записи предыдущей сессии (0xFFFF — нет)
    uint16_t prev_session;  // +16 индекс предыдущей сессии
} radex_arch_sess_t;
typedef struct {
    uint16_t last_gidx;
    uint16_t cur_session;
    uint8_t  n_sess;        // разобрано сессий
    bool     chain_ok;      // дошли до самой старой (prev_gidx == 0xFFFF)
    radex_arch_sess_t sess[RADEX_ARCH_MAX_SESS];
} radex_arch_hdr_t;
bool radex_parse_arch_hdr(const uint8_t *res, size_t n, radex_arch_hdr_t *out);

// Страница архива (0x0C0E): u32 длина, затем записи по 22 байта, номера по убыванию.
#define RADEX_ARCH_REC_LEN   22
#define RADEX_ARCH_PAGE_RECS 22
typedef struct {
    uint16_t raw0;        // @0  индекс сессии (трасса 04: 1 у новых, 0 у старых); страницы идут сквозь сессии
    uint16_t idx;         // @2  номер записи
    uint32_t time_s2000;  // @4  с от 2000-01-01 (местное)
    float    oa;          // @8  ОА, Бк/м3
    float    avg;         // @12 скользящее среднее, Бк/м3
    uint32_t t_x10;       // @16 температура ×10
    uint8_t  rh;          // @20 влажность, %
    uint8_t  flags;       // @21 (в трассе 0x10)
} radex_arch_rec_t;
// Возврат: число разобранных записей (до первой записи из одних 0xFF), -1 — результат короче 4 байт.
int radex_parse_arch_page(const uint8_t *res, size_t n, radex_arch_rec_t *out, int max);

// Календарь прибора: секунды / миллисекунды от 2000-01-01 00:00:00 (поля местного времени).
int64_t  radex_days_from_civil(int y, int m, int d);
uint32_t radex_s2000(int year, int mon, int day, int hh, int mm, int ss);
uint64_t radex_ms2000(int year, int mon, int day, int hh, int mm, int ss, int ms);
