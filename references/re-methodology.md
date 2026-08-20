# radex-esp32 — Reverse-методология для нового Radex-прибора (полное тело)

Извлечено из `SKILL.md` при lean-рефакторе #CTX-4 (2026-07-11), провенанс verbatim.
Приоритетный порядок шагов reverse engineering BLE-протокола приборов Quarta-Rad / Radex.

## Reverse-стратегия для нового Radex-прибора (приоритетный порядок)

### 1. btsnoop HCI capture от Android RadexM (основной путь)

1. Подключить прибор к телефону через RadexM-app.
2. Settings → Developer options → **Enable Bluetooth HCI snoop log** → ON.
3. Toggle Bluetooth off/on (новые сессии пишутся в новый лог).
4. Открыть RadexM, дать наработать **5+ минут** во всех режимах UI прибора
   (mgновенные значения, накопление, гистограммы — что есть).
5. Take bug report (`adb bugreport <out.zip>` или Developer options → Take bug report).
6. Из zip извлечь `FS/data/misc/bluetooth/logs/btsnoop_hci.log` (путь
   варьируется по производителям; на Xiaomi/POCO HyperOS — стандартный).
7. Парсер: Wireshark с диссектором `bluetooth → att`, либо собственный
   Python-парсер btsnoop (L2CAP reassembly + ATT opcode decode + GATT discovery
   aggregation) — пример скелета такого парсера: [joekickass/python-btsnoop](https://github.com/joekickass/python-btsnoop).
8. Зафиксировать новый `references/<model>.md` по шаблону mr107ion.md:
   GATT-таблица + handle-семантика + Read/Write/Notify статистика + open
   questions для следующей сессии.

### 2. ESP32 ble-explorer (верификация + 0x2901 descriptors)

Для разметки `handle → field name` btsnoop недостаточен — приложение использует
жёстко прошитые UUID, не читает User Description (0x2901). Скилл `ble-explorer`
(внутренний инструмент того же автора, публичного адреса нет) с прошивкой
`explorer.yaml v0.1.1+` через Web UI:
- BLE-scan → найти прибор по имени `MR107ion XXXX` / `RD1212 ...`.
- Подставить MAC в Web UI → Connect → GATT enum → подтвердить custom UUIDs.
- Прочитать дескрипторы 0x2901 каждой характеристики FE651600/FE651700 →
  получить человекочитаемые имена полей.
- Подписаться на Notify для контроля, что прибор молчит (= READ-poll, не push).

### 3. Косвенные источники

Quarta-Rad использует похожие encoding-паттерны в разных приборах:

| Прибор | Транспорт | URL |
|---|---|---|
| Radex One | USB serial | [mwwhited gist](https://gist.github.com/mwwhited) |
| RD1212, Radex One | USB | [luigifab/python-radexreader](https://github.com/luigifab/python-radexreader) |

Поведенческие гипотезы (требуют верификации):
- Little-endian uint16/uint32 counters.
- CRC-16 на хвосте payload'а (частая практика QuartaRad).
- Поля: timestamp, CPM/CPS, doseRate Bq/m³, температура, влажность,
  накопленная доза, battery.
