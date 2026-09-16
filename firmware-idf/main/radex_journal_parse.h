// #RADEX-281: журнал (история измерений) Radex MR107ion по Nordic UART.
// Чистый модуль без зависимостей от BLE/ESP-IDF — собирается и тестируется на
// хосте (test/host). Источник протокола: отчёт о захвате приложения 2026-09-16,
// раздел «Захват 3». Неподтверждённые поля хранятся СЫРЫМИ, не выбрасываются.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// GATT-карта NUS прибора (жёсткие handle, discovery не делаем — см. ble_radex.c).
#define RADEX_NUS_H_RX    0x0010   // write: команды
#define RADEX_NUS_H_TX    0x0012   // notify: ответы
#define RADEX_NUS_H_CCCD  0x0013   // CCCD уведомлений TX

#define RADEX_J_CMD_LEN   8

// Единственные разрешённые команды — дословно из захвата. Запись иного в RX
// недопустима: по NUS у прибора есть выключение, загрузчик, стирание.
extern const uint8_t RADEX_J_CMD_SUMMARY[RADEX_J_CMD_LEN];       // 47 00 ..
extern const uint8_t RADEX_J_CMD_SUMMARY_NEXT[RADEX_J_CMD_LEN];  // 81 ff 00 ..
extern const uint8_t RADEX_J_CMD_RECORDS[RADEX_J_CMD_LEN];       // 48 00 02 00 01 00 00 00
extern const uint8_t RADEX_J_CMD_RECORDS_NEXT[RADEX_J_CMD_LEN];  // 82 ff 00 ..

// true — только если (cmd,len) побайтно равен одной из четырёх команд выше.
bool radex_journal_cmd_allowed(const uint8_t *cmd, size_t len);
// true — только для значений CCCD 01 00 (включить notify) и 00 00 (выключить).
bool radex_journal_cccd_allowed(const uint8_t *val, size_t len);

#define RADEX_J_SUMMARY_LEN  20
#define RADEX_J_RECORD_LEN   24

// Пакет сводки (ответ на 47 + 81 ff), 20 байт.
typedef struct {
    uint16_t seq;         // @0  порядковый/счётчик пакета — гипотеза
    uint16_t raw2;        // @2  ? (в захвате 2)
    uint16_t raw4;        // @4  ? (в захвате 0)
    uint16_t last_record; // @6  номер последней записи — подтверждено
    uint32_t time_raw;    // @8  время последней записи, с, эпоха не установлена
    float    avg;         // @12 текущее среднее ОА? — гипотеза
    float    sko;         // @16 СКО/погрешность? — гипотеза
} radex_journal_summary_t;

// Запись журнала (ответ на 48 + 82 ff), 24 байта.
typedef struct {
    uint16_t seq;         // @0  порядковый/счётчик пакета — гипотеза
    uint16_t raw2;        // @2  ? (в захвате 0)
    uint16_t number;      // @4  номер записи — подтверждено
    uint32_t time_raw;    // @6  время, с — разность 600 с подтверждена, эпоха нет
    float    unk_f10;     // @10 ? — неизвестно
    float    oa;          // @14 ОА, Бк/м³ — подтверждено
    uint16_t temp_x10;    // @18 температура ×10 °C — подтверждено
    uint16_t raw20;       // @20 ? (в захвате 0)
    uint8_t  humidity;    // @22 влажность, % — подтверждено
    uint8_t  flags;       // @23 ? (в захвате 0x10)
} radex_journal_record_t;

// Результаты разбора пакета.
#define RADEX_J_OK      1    // поля заполнены
#define RADEX_J_FILLER  0    // пакет-заглушка: всё после первых двух байт равно 0xff
#define RADEX_J_SHORT  -1    // короче формата (например, MTU 23 обрезал до 20)
#define RADEX_J_BADARG -2

int radex_journal_parse_summary(const uint8_t *p, size_t len, radex_journal_summary_t *out);
int radex_journal_parse_record(const uint8_t *p, size_t len, radex_journal_record_t *out);

// Сырой пакет — для выдачи как есть (подбор параметров 0x48 на живой плате).
#define RADEX_J_RAW_MAX 32
typedef struct {
    uint8_t len;                 // сколько байт сохранено (<= RADEX_J_RAW_MAX)
    uint8_t orig_len;            // сколько пришло в notify (насыщение на 255)
    uint8_t data[RADEX_J_RAW_MAX];
} radex_journal_raw_t;

#define RADEX_J_MAX_PKT 8

// Сохранить пакет в raw (обрезка до RADEX_J_RAW_MAX).
void radex_journal_raw_store(radex_journal_raw_t *dst, const uint8_t *p, size_t len);

// Итог одного чтения журнала.
typedef struct {
    bool     valid;              // хоть один сеанс завершён (успешно или нет)
    bool     ok;                 // последний сеанс прошёл все шаги
    char     status[48];         // "ok" | "timeout: step N" | "disconnect" | ...
    uint32_t finished_s;         // аптайм завершения, с
    uint16_t mtu;                // ATT MTU соединения (23 = не согласован; записи нужен >= 27)
    bool     have_summary;
    radex_journal_summary_t summary;
    uint8_t  n_summary_pkt;
    radex_journal_raw_t summary_pkt[RADEX_J_MAX_PKT];
    uint8_t  n_records;
    radex_journal_record_t records[RADEX_J_MAX_PKT];
    uint8_t  n_record_pkt;
    radex_journal_raw_t record_pkt[RADEX_J_MAX_PKT];
} radex_journal_t;

// JSON для GET /api/journal. Возвращает длину (без нуля) или -1 при нехватке буфера.
int radex_journal_json(const radex_journal_t *j, bool busy, bool pending, char *buf, size_t len);
