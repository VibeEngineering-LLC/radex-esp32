---
name: radex-esp32
version: 0.3.0
description: >-
  Quarta-Rad / Radex по BLE (прил. RadexM): reverse протокола FE651Y00 + ESPHome-шлюз
  radex_gateway_s3.yaml для ESP32-S3 → HA/narodmon. MR107ion радон, READ-poll (не Notify).
  НЕ для AtomFast/RadonEye.
---

# radex-esp32 — Quarta-Rad / Radex: BLE-протокол + ESP32-шлюз

Прошивки `radex_gateway*.yaml` + результаты reverse engineering BLE-протокола линейки
Quarta-Rad / QuartaRad (Москва, [quartarad.com](https://quartarad.com), приложение
**RadexM** [Android](https://play.google.com/store/apps/details?id=ru.quartarad.radexm)).
Цель — интеграция приборов в ESP32-шлюзы (HA / Народмон) и собственный софт без штатного
приложения. Прежнее имя `radex-ble` (переименован 2026-06-13).

## ⚠️ SAFETY-CORE (HARD — читать ДО касания прошивки/железа)

1. **Никаких секретов в репо.** WiFi-пароль, MAC, OTA-пароль, API encryption key — ТОЛЬКО
   через `!secret` + `secrets.example.yaml`. **`.bin`/`.factory.bin` НЕ публиковать**:
   `strings firmware.bin` вытащит все ASCII-секреты. Пользователь собирает у себя.
2. **Народмон — HARD HOLD (бан аккаунта оператора).** Switch `narodmon_enabled` ОБЯЗАН иметь
   **`restore_mode: ALWAYS_OFF`** (первый параметр после `name:`) — после reboot/safe-mode/
   factory_reset/OTA switch гарантированно OFF. Никаких `RESTORE_DEFAULT_OFF`/`ALWAYS_ON`. НЕ
   включать switch / кнопку «Послать сейчас» без явного снятия бана оператором в чате.
3. **`esphome logs --device COMx` ЗАПРЕЩЕНО** — toggle'ит RTS/DTR → ребут ESP32 → обрыв
   BLE-сессии. Использовать OTA-logger или python-serial с `RTS/DTR off`.

## Актуальная прошивка `radex_gateway_s3.yaml` (step3, 2026-06-17)

**Плата ESP32-S3-DevKitC-1** (`esp32-s3-devkitc-1`, variant `esp32s3`, framework **arduino** —
esp-idf на S3 для arduino_3.0+ нестабилен). BLE-клиент к **Radex MR107ion** (READ-poll, не
Notify; OAR_last @ handle `0x0049` float LE, см. [`references/mr107ion.md`](references/mr107ion.md)).
Web Server v3 + sorting_groups + Basic Auth (`user=radex`) + API encryption для HA.
step1 sensor ОА радона / uptime / RSSI; step2 sliding-window (час 60×60с + день 1440×60с,
`filter_out: nan`, `web_server.log: true` — исключение из HARD-default); step3 Народмон-инфра
(ВЫКЛ по умолчанию, см. SAFETY-CORE #2). Полная история шагов — [`firmware/CHANGELOG.md`](firmware/CHANGELOG.md).

Старая плата ESP32-DevKitC (`firmware/radex_gateway.yaml` v0.3.0-step8, esp-idf) снята
оператором 2026-06-17, YAML оставлен как working snapshot для классического DevKitC.

## Когда использовать

- Reverse engineering нового BLE-прибора Quarta-Rad / Radex (определяется через RadexM).
- Интеграция Radex-прибора в ESP32-прошивку (`ble_client` / `esp32_ble_tracker`) или
  standalone-софт (`bleak` / `noble` / native ESP-IDF).
- Сборка BLE-шлюза Radex → Home Assistant / Народмон.
- Проверка гипотез о форматах payload перед калибровкой/деплоем; расширение скилла после
  reverse-сессии.

## Семейство приборов и статус reverse

| Прибор | Тип | BLE | Reverse-статус | Reference |
|---|---|---|---|---|
| **MR107ion** | Радон, ионизационная камера | + | **Полный GATT: 15 char сервиса FE651700, OAR_last @ 0x0049 float LE** (2026-06-13) | [references/mr107ion.md](references/mr107ion.md) |
| RD1212-BT | Гамма Geiger | + | reverse не делался; USB-донор [luigifab/python-radexreader](https://github.com/luigifab/python-radexreader) | — |
| Radex One | Гамма | USB-only | USB reverse [mwwhited gist](https://gist.github.com/mwwhited) | — |

Новая модель → `references/<model>.md` (шаблон mr107ion.md: GATT + handle-семантика + гипотезы payload).

## Общие паттерны линейки

- **GATT-база `FE651Y00-00B0-4240-BA50-05CA45BF8AAA`** — фирменная схема Quarta-Rad: младшая
  цифра `Y` = functional cluster (`6` config R/W, `7` measurement R). NUS-сервис
  (`6E400001-...`) — **placeholder, не работает** (приложение игнорирует).
- **READ-poll, не Notify** — приложение опрашивает handles на 5 Hz; **0 Notify пакетов** за
  сессию (противоположно RadonEye/AtomFast push-only). ESP32-port: async-listener не нужен,
  период поллинга любой (≥1 мин норм для радона).
- Bonding/pairing **не требуются**. Battery Service `0x180F` у MR107ion **не найден** (заряд
  спрятан в custom config, проверять через 0x2901).

Полные тела (GATT-таблица, детали паттернов) — [`references/family-patterns.md`](references/family-patterns.md).

## Reverse-стратегия (приоритетный порядок)

1. **btsnoop HCI capture** от Android RadexM (основной путь): Developer options → HCI snoop
   log → ON, наработать 5+ мин, `adb bugreport` → `btsnoop_hci.log` → Wireshark/Python-парсер.
2. **ESP32 [ble-explorer](https://github.com/Verter73/claude-skills/tree/master/ble-explorer)** — верификация UUID + чтение 0x2901 дескрипторов (btsnoop их не содержит).
3. **Косвенные источники** (USB-доноры Radex One / RD1212) для encoding-гипотез.

Полные шаги (btsnoop-извлечение, ble-explorer-проход, гипотезы payload) — [`references/re-methodology.md`](references/re-methodology.md).

### Anti-patterns RE (не safety — методология)

- ❌ Не путать **MR107** (USB-only) и **MR107ion** (BLE) — USB-инструменты к BLE не подходят.
- ❌ Не ждать Notify от NUS — placeholder, CCCD writes игнорируются молча.
- ❌ 0x2901 дескрипторы RadexM в btsnoop не читает → нужен отдельный ble-explorer проход.

## Связанные скиллы

- **[ble-explorer](https://github.com/Verter73/claude-skills/tree/master/ble-explorer)** — in-field verification GATT-карт + Notify ring buffer.
- **[atomfast-esp32](https://github.com/VibeEngineering-LLC/atomfast-esp32)** — параллельный шлюз AtomFast (Notify-based).
- **RadonEye Plus2** (FTLAB) — радон-детектор, Notify-based, отдельный C3-шлюз.

## Структура скилла

```
radex-esp32/
├── SKILL.md · README.md · INSTALL.md · LICENSE (MIT)
├── firmware/
│   ├── radex_gateway_s3.yaml          ← актуальная S3 (Web UI v3, step3 Народмон ВЫКЛ)
│   ├── radex_gateway_s3_baseline.yaml ← baseline S3 для smoke-теста
│   ├── radex_gateway.yaml             ← старая классика ESP32-DevKitC (esp-idf, v0.3.0-step8)
│   ├── radex_gateway_v2.yaml          ← альт. UX классики (Web Server v2, плоская таблица)
│   ├── secrets.example.yaml · CHANGELOG.md · archive/
└── references/
    ├── mr107ion.md          ← полная reverse-таблица MR107ion (GATT, handles, byte map)
    ├── family-patterns.md   ← общие паттерны линейки (полное тело)
    └── re-methodology.md    ← reverse-стратегия (полные шаги)
```

## Reference-файлы

| Файл | Содержимое |
|---|---|
| [`references/mr107ion.md`](references/mr107ion.md) | Полная reverse-таблица MR107ion (GATT, handles, byte map) |
| [`references/family-patterns.md`](references/family-patterns.md) | Общие паттерны линейки (GATT-структура, READ-poll, bonding, battery) |
| [`references/re-methodology.md`](references/re-methodology.md) | Reverse-стратегия (btsnoop capture, ble-explorer, косвенные источники) |
