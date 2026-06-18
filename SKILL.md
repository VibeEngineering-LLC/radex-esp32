---
name: radex-esp32
version: 0.3.0
description: |
  Reverse engineering и интеграция приборов Quarta-Rad / Radex по BLE (Android-приложение RadexM).
  Семейство: радон-детекторы и дозиметры с общей GATT-схемой FE651Y00-... + custom services.
  Помимо протокола, скилл содержит ESPHome-прошивки `radex_gateway*.yaml` для
  ESP32-S3-DevKitC-1 (актуальная, step3) и ESP32-DevKitC (старая, v0.3.0-step8 esp-idf).
  Подробная история — `firmware/CHANGELOG.md`. Прежнее имя: radex-ble.
---

# radex-esp32 — Quarta-Rad / Radex: BLE-протокол + ESP32-шлюз

## Актуальная прошивка `radex_gateway_s3.yaml` step3 (2026-06-17)

**Целевая плата:** **ESP32-S3-DevKitC-1** (board `esp32-s3-devkitc-1`, variant `esp32s3`,
framework **arduino** — esp-idf на Windows без MSYS2 не работает; на S3 esp-idf для
arduino_3.0+ нестабилен).

Старая плата ESP32-DevKitC (`firmware/radex_gateway.yaml` v0.3.0-step8, esp-idf;
последний шаг — `bluetooth_proxy` закомментирован для изоляции BLE-стабильности)
снята оператором 2026-06-17 — заменена на S3 целиком. YAML оставлен в скилле как
working snapshot для тех, у кого ещё классический DevKitC.

Функции:
- BLE-клиент к **Radex MR107ion** (READ-poll, не Notify; OAR_last @ handle 0x0049
  float LE, см. `references/mr107ion.md`);
- Web Server v3 + sorting_groups + Basic Auth (`user=radex`);
- API encryption для Home Assistant;
- **step1**: sensor ОА радона (instant), uptime, BLE RSSI;
- **step2**: sliding-window averages — час (60 сэмплов × 60 с) + день (1440 × 60 с),
  `filter_out: nan` для отсечки BLE-пропусков; включён `web_server.log: true`
  (Debug Log виджет в Web UI v3, исключение из HARD-default `log: false`);
- **step3 (2026-06-17): Народмон-инфраструктура, по умолчанию ВЫКЛ**:
  - `http_request` (verify_ssl: false, timeout 5s);
  - `switch narodmon_enabled` — **`restore_mode: ALWAYS_OFF`** (HARD safety);
  - `button narodmon_send_now` — ручной триггер;
  - `select narodmon_method` — `HTTP GET / HTTP POST / HTTPS POST / JSON POST`
    (дефолт `HTTP GET`);
  - `text nm_radon=RR1 / nm_temp=T1 / nm_hum=H1`;
  - `script send_narodmon` (4 if-ветви, guarded);
  - `interval 300s` — авто-отправка ТОЛЬКО при `narodmon_enabled.state == true`.

**HARD-правило проекта:** switch Народмона ОБЯЗАН иметь `restore_mode: ALWAYS_OFF`
во всех прошивках — после reboot/safe-mode/factory_reset switch гарантированно OFF.

Параллельный BLE-шлюз для **RadonEye Plus2** ведётся в отдельном скилле автора —
обвязка та же (XIAO ESP32-C3 + ESPHome + Notify-based декодер).

---

Переименован из `radex-ble` 2026-06-13: добавлена ESPHome-прошивка
для постоянного шлюза. BLE-разведку оставляем — это базовая часть скилла.

Семейство бытовых детекторов радиации производства **Quarta-Rad / QuartaRad** (Москва, RU,
[quartarad.com](https://quartarad.com)) с поддержкой BLE и единым приложением **RadexM**
([Android](https://play.google.com/store/apps/details?id=ru.quartarad.radexm),
[iOS](https://apps.apple.com/us/app/radexm/id1524841479)).

Скилл содержит результаты reverse engineering BLE-протокола приборов этой линейки —
GATT-таблицы, паттерны взаимодействия host↔device, опкоды/handles ключевых
характеристик, гипотезы по форматам payload'ов. Цель — интеграция приборов в
ESP32-шлюзы (Home Assistant, Народмон) и в собственный софт без штатного
приложения.

## Когда использовать этот скилл

- Reverse engineering нового BLE-прибора Quarta-Rad / Radex (любой, что
  определяется через приложение RadexM).
- Интеграция Radex-прибора в ESP32-прошивку (ESPHome `ble_client` / `esp32_ble_tracker`)
  или в standalone-софт через `bleak` / `noble` / native ESP-IDF.
- Сборка bluetooth-шлюза Radex → Home Assistant / Народмон.
- Проверка предположений о форматах payload, periodicity, encoding'ах перед
  калибровкой/боевым деплоем.
- Расширение скилла после reverse-сессии очередного Radex-прибора.

## Семейство приборов и статус reverse

| Прибор | Тип | BLE | RadexM-app | Reverse-статус | Reference |
|---|---|---|---|---|---|
| **MR107ion** | Радон, ионизационная камера | + | + | **Полный GATT-профиль декодирован: 15 char сервиса FE651700, OAR_last @ 0x0049 float LE** (2026-06-13) | [references/mr107ion.md](references/mr107ion.md) |
| RD1212-BT | Гамма-дозиметр Geiger | + | + | reverse не делался; есть USB-донор [luigifab/python-radexreader](https://github.com/luigifab/python-radexreader) | — |
| Radex One | Гамма, BLE? | — (USB-only) | — | USB reverse доступен [mwwhited gist](https://gist.github.com/mwwhited) | — |

Новые приборы добавляются как `references/<model>.md` с обязательной таблицей
GATT + handle-семантикой + гипотезами payload'а (формат — см. `mr107ion.md`).

## Общие паттерны линейки (по mr107ion + наблюдениям)

### Структура GATT

| Сервис | UUID | Назначение |
|---|---|---|
| GAP | `0x1800` | стандарт |
| GATT | `0x1801` | стандарт |
| **Nordic UART** | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | **placeholder, не работает** (приложение игнорирует) |
| **Device Information** | `0x180A` | стандарт, manufacturer/model/serial |
| **Custom Config** | `FE651Y00-00B0-4240-BA50-05CA45BF8AAA`, `Y=6` | R/W, configuration handles |
| **Custom Measurement** | `FE651Y00-...`, `Y=7` | R, measurement handles |

База `FE651Y00-...` — фирменная схема Quarta-Rad: одна группа = один functional cluster,
младшая цифра `Y` различает (`6` config, `7` measurement, потенциально другие для будущих
приборов).

### Pattern взаимодействия — READ-poll, не Notify

Все наблюдённые приборы Radex с BLE используют **поллинг через ATT Read**, а не push
через Notify. Приложение опрашивает ключевые handles на 5 Hz, формально подписываясь на
Notify NUS-сервиса (CCCD write `0100`), но **0 Notify пакетов** приходит за сессию.

Это противоположно RadonEye (push-only Notify) и AtomFast (push-only). Преимущество для
ESP32-port:
- не нужен async Notify-listener;
- можно поллить с любым удобным периодом (≥1 мин нормально для физики радона);
- меньше нагрузки на BLE-стек ESP32.

### Bonding

Bonding и pairing **не требуются** ни для одного известного прибора линейки.

### Battery service

Стандартный Battery Service (`0x180F`) у MR107ion **не найден** — индикатор батареи,
вероятно, спрятан в custom config-сервисе. Аналогично может быть на других моделях
линейки. Проверять через 0x2901 (User Description) handles config-сервиса.

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
жёстко прошитые UUID, не читает User Description (0x2901). Скилл [ble-explorer](https://github.com/Verter73/claude-skills/tree/master/ble-explorer)
с прошивкой `explorer.yaml v0.1.1+` через Web UI:
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

## Связанные скиллы

- **[ble-explorer](https://github.com/Verter73/claude-skills/tree/master/ble-explorer)** —
  ESP32-прошивка для in-field verification GATT-карт и захвата Notify ring buffer.
- **[atomfast-esp32](https://github.com/VibeEngineering-LLC/atomfast-esp32)** —
  параллельный публичный шлюз для дозиметра AtomFast (BLE Notify-based, открытый
  протокол).
- **RadonEye Plus2** (соседнее семейство FTLAB) — также радон-детектор, но
  Notify-based, не READ-poll; параллельный наш шлюз C3-based.

## Anti-patterns / pitfalls

- ❌ Не путать **MR107** (USB-only, старая ревизия) и **MR107ion** (BLE).
  USB reverse-инструменты к BLE-ревизии не подходят.
- ❌ Не ждать Notify от NUS-сервиса — он у MR107ion placeholder, прибор
  молча игнорирует CCCD writes. Та же гипотеза вероятна для других Radex-приборов
  с BLE: NUS объявлен для совместимости, реальная работа — через custom services.
- ❌ Не читать дескрипторы 0x2901 через приложение RadexM в btsnoop — оно их
  не запрашивает. Нужен отдельный ble-explorer проход.
- ❌ `esphome logs --device COMx` toggle'ит RTS/DTR → перезагружает ESP32 →
  обрывает BLE-сессию. Использовать OTA-logger или python-serial с
  `RTS/DTR off`.
- ❌ **Бинарники прошивок (.bin / .factory.bin) НЕ публикуются** в этот скилл.
  После `esphome compile` файл `.pioenvs/<device>/firmware.bin` содержит WiFi SSID +
  пароль, MAC устройств, OTA-пароль, API encryption key в виде ASCII-строк (любой
  может `strings firmware.bin`). Только YAML + `secrets.example.yaml` — пользователь
  собирает у себя.
- ❌ Switch Народмона **без** `restore_mode: ALWAYS_OFF`. HARD-правило проекта: в любой
  прошивке с Народмон-инфраструктурой выключатель ОБЯЗАН иметь этот restore_mode —
  после reboot/safe-mode/factory_reset/OTA switch гарантированно вернётся в OFF.
  Никаких `RESTORE_DEFAULT_OFF`, `ALWAYS_ON`. Образец — `radex_gateway_s3.yaml` step3.

## Структура скилла

```
radex-esp32/
├── SKILL.md                              ← этот файл (обзор семейства + методология RE)
├── README.md                             ← описание прошивок radex_gateway*.yaml и quickstart
├── INSTALL.md                            ← подробная пошаговая установка с нуля
├── LICENSE                               ← MIT
├── firmware/
│   ├── radex_gateway_s3.yaml             ← актуальная прошивка ESP32-S3 (Web UI v3, step3 Народмон ВЫКЛ)
│   ├── radex_gateway_s3_baseline.yaml    ← baseline S3 для smoke-теста железа
│   ├── radex_gateway.yaml                ← старая прошивка для классического ESP32-DevKitC (esp-idf, v0.3.0-step8)
│   ├── radex_gateway_v2.yaml             ← альтернативный UX для классики (Web Server v2, плоская таблица)
│   ├── secrets.example.yaml              ← placeholder-секреты
│   ├── CHANGELOG.md                      ← история прошивок (semver, по шагам)
│   └── archive/                          ← снимки прошлых стабильных версий
└── references/
    └── mr107ion.md                       ← полная reverse-таблица MR107ion (GATT, handles, byte map)
```

При reverse'е новой модели — добавлять `references/<model>.md` и расширять
таблицу в секции «Семейство приборов и статус reverse» этого SKILL.md.
