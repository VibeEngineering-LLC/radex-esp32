# Radex MR107ion — BLE reverse engineering notes

**Источник:** оператор сообщил 2026-06-12, что для прибора в эфире
`C1:XX:XX:XX:XX:XX` (`MR107ion 0214`, NUS service `6E400001-...`) есть
официальное Android-приложение.

## Цель и приложение

| Поле | Значение |
|---|---|
| Модель устройства | **RADEX MR107ion** (новая ревизия MR107 с BLE) |
| Производитель | Quarta-Rad / QuartaRad (Москва, RU; quartarad.com) |
| Тип | Радон-детектор (ионизационная камера) |
| Android app | `ru.quartarad.radexm` — https://play.google.com/store/apps/details?id=ru.quartarad.radexm |
| iOS app | "RadexM" — https://apps.apple.com/us/app/radexm/id1524841479 (bundle `id1524841479`) |
| Поддерживаемые приложением | MR107ion (подтверждено описанием store), возможно RD1212-BT |
| Канал | BLE Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`) |
| Pairing/bonding | НЕ требуется (подтверждено оператором) |

## Что наблюдается в эфире (от ble-explorer v0.1.0)

```
C1:XX:XX:XX:XX:XX  RSSI -55..-59 dBm  name="MR107ion 0214"
  svc: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E   ← Nordic UART Service
```

Standard NUS layout (ожидается):
- `6E400002-...` — RX (Write/Write No Response, host → device): команды
- `6E400003-...` — TX (Notify, device → host): данные / ответы

## GATT-таблица + поведение из btsnoop (2026-06-12, RadexM Android)

**Источник:** Live HCI capture с телефона оператора (POCO F5, HyperOS) во время
работы официального приложения `ru.quartarad.radexm`. Файл
`btsnoop_hci_260612_181501.log` (7.5 МБ, 46 840 записей), сессия с прибором
`C1:XX:XX:XX:XX:XX` длилась ~27 минут (18:25:38 – 18:52 MSK).

Парсер: Wireshark с диссектором `bluetooth → att`, либо собственный Python-парсер
btsnoop (L2CAP reassembly, ATT opcode decode, GATT discovery aggregation) —
пример скелета: [joekickass/python-btsnoop](https://github.com/joekickass/python-btsnoop).

### Полная GATT-таблица MR107ion

| Handle range | Service UUID | Назначение |
|---|---|---|
| `0x0001–0x0009` | `0x1800` | GAP (стандарт) |
| `0x000a–0x000d` | `0x1801` | GATT (стандарт) |
| `0x000e–0x0013` | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Nordic UART (NUS) — **placeholder, не работает** |
| `0x0014–0x0020` | `0x180a` | Device Information |
| `0x0021–0x003d` | `FE651600-00B0-4240-BA50-05CA45BF8AAA` | Custom Config (R/W, 14 chars) |
| `0x003e–0x???` | `FE651700-00B0-4240-BA50-05CA45BF8AAA` | Custom Measurement (R, 15 chars) |

Основная работа — через два custom-сервиса с базой
`FE651Y00-00B0-4240-BA50-05CA45BF8AAA`, где `Y=6` для конфигурации и `Y=7` для
измерений (стандартная QuartaRad-схема: сервис на функциональную группу).

### Pattern взаимодействия — READ-poll, **НЕ** Notify (главная находка)

**Приложение опрашивает прибор циклическими ATT Read, не подписывается на Notify.**
За 27-минутную сессию:

- Total ATT Read requests: **5 184** (≈ 3.2 req/sec средне).
- Каждый ключевой handle опрошен **324 раза** (≈ один раз в 5 секунд per handle).
- Подписка на Notify (CCCD write `0100`): только **2 раза** на handle `0x0013` (NUS
  TX descriptor), и за весь сеанс **0 Notify пакетов** не пришло — прибор честно
  отвергает Notify, потому что NUS-сервис — заглушка.

Это сильно отличается от RadonEye / AtomFast (push-only), но облегчает ESP32-port:
не нужно ждать асинхронные Notify, можно поллить с любым удобным периодом.

### Живые handles (5 Hz, по 324 чтения каждый — из btsnoop)

| Handle | Назначение (гипотеза по динамике) | Формат | Пример значения |
|---|---|---|---|
| `0x0046` | Uptime (секунды от boot) | LE uint32 | монотонно растёт |
| `0x0049` | **Радон последний (OAR_last)** | LE float32 | 69.0 Бк/м³ (см. полную карту ниже) |
| `0x0025` | Config flag | uint8 | `0x0a` (стабильно) |
| `0x0027` | Config flag | uint8 | `0x05` (стабильно) |
| `0x0031` | Status flag | uint8 | `0x01` (стабильно) |

> **Update 2026-06-13**: ранняя гипотеза «радон = 0x0067» (по частоте чтения в
> btsnoop) **опровергнута** ble-explorer sweep'ом v0.1.4 + v0.1.5: 0x0067 — это
> поле `t_prepare` (u16, всегда 0 в наших условиях). Реальный радон **OAR_last**
> сидит на handle **0x0049** (LE float, в Бк/м³). Полная карта 15 характеристик
> сервиса FE651700 — ниже в секции «GATT-15: полный профиль».

## GATT-15: полный профиль сервиса FE651700 (2026-06-13)

**Источник:** ble-explorer v0.1.4 (sweep even handle 0x003E..0x00A0 step 2) +
v0.1.5 (sweep odd handle 0x003F..0x006B step 2). Логи:
`firmware/ble-explorer/.logs/sweep_v0.1.{4,5}_90s.log` в ESP32 workspace.

### Главная находка

Каждая характеристика сервиса `FE651700-00B0-4240-BA50-05CA45BF8AAB` занимает
**ровно 3 последовательных handle**:

1. **Decl** (нечётный начиная с 0x003F) — Properties + value_handle + 128-bit UUID
   с базой `FE6517NN-00B0-4240-BA50-05CA45BF8AAB`, где `NN` = 01, 02, 03 ... 15
   (hex, не строго возрастает: пропуски 0A-0F вероятно из-за hex/dec путаницы
   разработчика).
2. **Value** (decl+1) — собственно данные (float32 LE / uint32 LE / uint16 LE / uint8).
3. **User Description** (decl+2, UUID 0x2901) — **человекочитаемое имя поля** ASCII
   (транслит + английский, почерк русскоязычной прошивки).

Это делает протокол **самодокументированным**: сначала прочитал desc-handle,
получил имя поля; потом читай value по offset −1.

### Полная карта (живые значения от MR107ion 0214)

| # | Decl | Value | Desc | Имя | Тип | Сырое значение | Декод | Смысл |
|---|---|---|---|---|---|---|---|---|
| 1 | 0x003F | **0x0040** | 0x0041 | `OAR_sred` | float32 LE | `55 89 93 42` | **73.79** | Радон среднее, Бк/м³ |
| 2 | 0x0042 | **0x0043** | 0x0044 | `Sko_OAR_sred` | float32 LE | `A1 2A 39 41` | **11.572** | СКО среднего, Бк/м³ |
| 3 | 0x0045 | **0x0046** | 0x0047 | `t_izm_last` | uint32 LE | `8B 58 00 00` | **22667** | Uptime / время посл. изм., сек (+1/сек) |
| 4 | 0x0048 | **0x0049** | 0x004A | **`OAR_last`** | float32 LE | `00 00 8A 42` | **69.0** | **Радон последнее, Бк/м³** ✅ |
| 5 | 0x004B | **0x004C** | 0x004D | `OAR_mov_avr` | float32 LE | `55 55 92 42` | **73.17** | Скользящее среднее, Бк/м³ |
| 6 | 0x004E | **0x004F** | 0x0050 | `Qakk` (trunc) | uint16 LE | `61 00` | **97** | Имя обрезано до 4 байт; вероятно `Qakkum_OAR` |
| 7 | 0x0051 | **0x0052** | 0x0053 | `OAR_min` | float32 LE | `7C 3B 47 42` | **49.81** | Радон минимум, Бк/м³ |
| 8 | 0x0054 | **0x0055** | 0x0056 | `OAR_max` | float32 LE | `23 F7 99 42` | **76.98** | Радон максимум, Бк/м³ |
| 9 | 0x0057 | **0x0058** | 0x0059 | `temper_x10` | int16 LE *) | `0A 01` | **266** | Температура × 10 → **26.6 °C** |
| 10 | 0x005A | **0x005B** | 0x005C | `preassure` (sic) | uint16 LE | `00 00` | 0 | Давление (пока ещё не считалось/нет датчика?) |
| 11 | 0x005D | **0x005E** | 0x005F | `humidity` | uint8 | `31` | **0x31 = 49** | Влажность, % |
| 12 | 0x0060 | **0x0061** | 0x0062 | `quantity_izm` | uint16 LE | `04 00` | 4 | (количество единиц измерения?) |
| 13 | 0x0063 | **0x0064** | 0x0065 | `num_izmer` | uint16 LE | `03 00` | 3 | Число измерений |
| 14 | 0x0066 | **0x0067** | 0x0068 | `t_prepare` | uint16 LE | `00 00` | 0 | Время подготовки (сек?) |
| 15 | 0x0069 | **0x006A** | 0x006B | `mis_psw` | uint16 LE | `80 00` | 128 | misc / status flags? |

### Sanity-checks (внутренняя согласованность)

- `OAR_last` (69.0) укладывается в диапазон min/max (49.81 / 76.98) и близко к
  среднему/скользящему (73.79 / 73.17) — все значения консистентны.
- `temper_x10` = 266 → 26.6 °C, `humidity` = 49 % — типовые комнатные условия.
- `t_izm_last` растёт на +1 в секунду (+25 за 25 c наблюдения) — uptime.

#### *) Знаковость `temper_x10` — uint16 vs int16

В btsnoop'ах от прибора 0214 наблюдались только положительные сэмплы
температуры. Битовое представление для значений 0..32767 у `uint16` и `int16`
two's complement совпадает, поэтому различить тип по этим данным нельзя.
Шлюз декодирует поле как `int16 LE` — это **safe super-set**:

- Если истинный тип в прошивке прибора — `uint16` без отрицательных значений
  (что соответствует наблюдениям), int16-декодер для всех известных кадров
  выдаёт идентичный результат.
- Если истинный тип — `int16` two's complement (отрицательная температура
  будет однажды передана), uint16-декодер дал бы ~65500/10 = 6550 °C на
  сэмпле −20 °C, и `guard t<=85 °C` отсёк бы такой кадр (температура
  пропадала бы из Web UI). int16-декодер корректно даст −20 °C и пройдёт
  через guard −40..+85 °C.

Подтвердить дефинитивно (uint16 или int16) сможет только сэмпл с
отрицательной температурой (морозильник / зимний балкон) или статья
от Quarta-Rad. До этого момента int16 — корректный выбор для обоих
случаев. Помечено в `firmware/include/radex_read_hook.h` и в YAML-комментариях.

#### Верхний предел шкалы радона MR107ion

Top-of-scale прибора (максимум, выше которого float-показание считается
артефактом) **не задокументирован Quarta-Rad** в открытых источниках —
ни в инструкции, ни на сайте quartarad.com, ни в `info.plist` приложения
RadexM. В прошивках шлюза радон-guard оставлен только на `std::isfinite(v) && v >= 0`:
отсекает NaN-кадры и отрицательные мусорные значения, но не накладывает
верхний лимит «сверху знать нельзя». Если когда-то найдётся датасайт или
будет проведён прямой эксперимент в калибровочной камере — поднять верхний
лимит явно и убрать эту заметку.

### Минимальный poll-set для ESPHome-port

Чтобы построить рабочий шлюз, достаточно 4-5 read'ов на цикл:

| Handle | Поле | Формат | Сенсор |
|---|---|---|---|
| **0x0049** | OAR_last | float32 LE | `radon_bqm3` — главное значение |
| 0x0040 | OAR_sred | float32 LE | `radon_avg_bqm3` |
| 0x0058 | temper_x10 | int16 LE / 10 *) | `temperature_c` |
| 0x005E | humidity | uint8 | `humidity_pct` |
| (опц.) 0x0046 | t_izm_last | uint32 LE | `uptime_sec` |

Period: с физики прибора (ионизационная камера, интегральная статистика по
минутным окнам) частые опросы не дают новых данных — то же значение возвращается
несколько раз. В прошивках скилла стоит 4.3 с round-robin на 4 хэндла (период
обновления каждого ≈17.2 с). Для standalone-софта 30-60 с — разумный практичный
дефолт; точное оптимальное значение зависит от того, какие именно поля и как
часто нужны.

### UUID характеристик (для `ble_client.characteristic`)

База: `FE6517NN-00B0-4240-BA50-05CA45BF8AAB` (последний байт `NN`):

| NN | Имя | Field |
|---|---|---|
| `01` | FE651701-... | OAR_sred (handle 0x0040) |
| `02` | FE651702-... | Sko_OAR_sred (0x0043) |
| `03` | FE651703-... | t_izm_last (0x0046) |
| `04` | FE651704-... | **OAR_last (0x0049)** |
| `05` | FE651705-... | OAR_mov_avr (0x004C) |
| `06` | FE651706-... | Qakk (0x004F) |
| `07` | FE651707-... | OAR_min (0x0052) |
| `08` | FE651708-... | OAR_max (0x0055) |
| `09` | FE651709-... | temper_x10 (0x0058) |
| `10` (=0x0A пропущен, в эфире 0x10) | FE651710-... | preassure (0x005B) |
| `11` | FE651711-... | humidity (0x005E) |
| `12` | FE651712-... | quantity_izm (0x0061) |
| `13` | FE651713-... | num_izmer (0x0064) |
| `14` | FE651714-... | t_prepare (0x0067) |
| `15` | FE651715-... | mis_psw (0x006A) |

> ESPHome `ble_client.characteristic` принимает UUID или handle. Handle на
> наблюдаемой ревизии 0214 совпал между двумя reset'ами, но межревизионная
> стабильность не верифицирована — UUID более устойчивый контракт (он зашит в
> прошивку и переживает реорганизацию GATT-таблицы). В скилле сейчас используется
> read-by-handle (быстрее, проще lambda), потому что под этот прибор реверс
> делался на одной ревизии. При портировании на новую ревизию или другой
> Radex-прибор — пересобрать GATT-карту через ble-explorer (0x2901 descriptors)
> и проверить handle'ы; если расходятся — переключиться на UUID-read.

### Подтверждение, что NUS — placeholder

Sweep'ы v0.1.4/v0.1.5 не пытались тыкать в NUS handle (0x000E..0x0013) — но
btsnoop уже подтвердил: CCCD-write на 0x0013 принимается, Notify не приходит.
NUS-сервис у MR107ion декларирован для совместимости/обнаружения, фактически
не работает.

### User Description descriptors (0x2901)

Каждая custom characteristic имеет дескриптор
`0x2901 (Characteristic User Description)` — handles `0x0041, 0x0044, ... 0x006b`.
**btsnoop НЕ содержит чтений descriptor-ов** — приложение их не вытягивает,
использует жёстко прошитые UUID. Поэтому для первичной разметки `handle → field
name` нужен отдельный ble-explorer проход, который читает 0x2901 каждой
характеристики. Это и есть назначение текущего ble-explorer smoke-теста.

### CCCD writes — видны, но не работают

```
18:25:42.472  WriteReq h=0x0013 value=0100  (subscribe NUS-TX Notify)
18:25:42.534  WriteReq h=0x0013 value=0100  (retry, identical)
```
Оба запроса прибор молча игнорирует. NUS у MR107ion — placeholder, не пытаться
получать данные через него.

### Что это даёт ESP32-port'у

- **Не нужен Notify-listener** — `esp_ble_gattc_read_char()` с периодом N секунд хватит.
- **Минимальный set handles**: `0x46` (uptime) + `0x67` (радон) — два read'a на цикл.
- **Period**: 60 сек норм (приложение поллит 5 Hz для UI-плавности, но физика радона
  — минуты-часы, чаще не нужно).
- **Bonding не нужен** — приложение работает без него, ESPHome тоже.

### Радио-контекст (для атрибуции #43 24h-прогона)

В той же btsnoop-сессии (18:40:22.748 MSK) телефон **HyperOS автоматически**
переподключился к AtomFast (MAC замаскирован, `AA:BB:CC:DD:EE:FF`) — через 7 секунд после того, как
atomfast-gateway потерял его (DISCONNECT #7 в #43 24h-прогоне). Это **не баг
прошивки v0.6.0**, а environmental issue: AtomFast имеет 1 peripheral slot, и
phone OS auto-reconnect bonded BLE девайсов крадёт его у ESP32. Для чистого
прогона #43 — выключить BT на телефоне или unbond AtomFast.

## Reverse-стратегия (priority order)

### 1. btsnoop HCI capture от Android (ОСНОВНОЙ ПУТЬ)

Самый быстрый и точный способ — снять `btsnoop_hci.log` через Android Developer
options, пока штатное приложение разговаривает с прибором:

1. На телефоне: Settings → Developer options → **Enable Bluetooth HCI snoop log** → ON.
2. Toggle Bluetooth off/on (новые сессии пишутся в новый лог).
3. Открыть приложение RadexM, подключиться к MR107ion, дать наработать
   **3-5 минут** во всех доступных режимах (instant cps, накопление,
   гистограмма — что есть в UI).
4. Снять bug report: Settings → System → Developer options → **Take bug report**
   → Interactive (или `adb bugreport <out.zip>`).
5. В zip найти `FS/data/misc/bluetooth/logs/btsnoop_hci.log` (путь варьируется
   по производителям и Android-версиям).
6. Анализ через Wireshark (`bluetooth → att`) или `python-btsnoop`:
   - GATT-таблица (Discover Services / Characteristics).
   - Write на RX-handle: команды host → device (init, request data, etc.).
   - Notify с TX-handle: payload device → host (cps, dose, status).

### 2. ESP32 ble-explorer (запасной путь — для верификации)

Через Web UI v3 (`http://<radex-ip>/`, IP взять из роутера или из UART-лога при первом boot) или REST API:
- Connect по MAC `C1:XX:XX:XX:XX:XX`
- GATT enum → подтвердить RX/TX UUIDs
- Subscribe на TX (`6E400003-...`)
- Захват Notify ring buffer (256 пакетов) → JSONL для анализа

Скрипт автоматизации: `scripts/auto_decode_radex.py` (8.7 КБ, готов к запуску).

### 3. Косвенные источники (encoding hints от QuartaRad)

QuartaRad использует похожие encoding-паттерны в разных приборах. Полезные
reverse-работы по соседним моделям:

| Источник | Прибор | Транспорт | URL |
|---|---|---|---|
| mwwhited gist | Radex One | USB serial | https://gist.github.com/mwwhited |
| luigifab/python-radexreader | RD1212, Radex One | USB | https://github.com/luigifab/python-radexreader |
| sormy/radoneye | Radon Eye | BLE | https://github.com/sormy/radoneye |

Поведенческие гипотезы (требуют верификации на реальных байтах):
- Little-endian uint16/uint32 counters.
- CRC-16 на хвосте пакета (QuartaRad — частая практика).
- Periodic broadcast вместо req/resp (типично для радон-детекторов: long
  averaging интервалы → push-only).
- Поля: timestamp, CPM/CPS, doseRate Bq/m³, температура, влажность,
  накопленная доза, battery.

### Anti-patterns / pitfalls

- ❌ Не путать MR107 (USB-only старый) и MR107ion (BLE-новый). Старые
  USB reverse-инструменты не подходят.
- ❌ `esphome logs --device COM9` toggle'ит RTS/DTR → перезагружает ESP32 →
  обрывает BLE-сессию посередине. Использовать OTA-logger (без `--device`)
  или `mode COM9 DTR=OFF RTS=OFF` + python-serial.
- ❌ SSE `/events` ESPHome возвращает 0 байт, когда main loop блокирован
  BLE connect attempt'ом. Симптом — пустой response из `/events` без
  ошибки. Решение — таймаут + retry, либо capture через DEBUG-logger
  по USB.

## Известные сторонние модели в линейке RadexM app

- **RD1212-BT** — гамма-дозиметр Geiger с BLE (отдельная reverse-работа доступна по python-radexreader).
- **MR107ion** — наша цель.

## TODO / open questions (update 2026-06-12)

### Закрыто btsnoop-сессией

- [x] ~~Подтвердить fixed RX/TX UUIDs через btsnoop / GATT enum~~ — NUS есть,
  но не используется. Реальная работа — через custom services FE651600/FE651700.
- [x] ~~Какие команды отправляет приложение при connect~~ — discovery + два
  безответных CCCD-write на NUS. Дальше — чистый READ-poll 5 Hz, **никаких write-команд** в основном цикле.
- [x] ~~Push-only или req/resp~~ — **req/resp**, инициатива у host'а.
- [x] ~~Частота Notify~~ — 0 Notify за сессию, прибор только отвечает на Read.
- [x] ~~Device info (0x180A)~~ — присутствует (handles 0x14-0x20).

### Осталось проверить (требует ble-explorer на собранной плате)

- [ ] Прочитать 0x2901 (User Description) для всех handles `0x21-0x6f` — получить
  человекочитаемые имена полей.
- [ ] Подтвердить семантику handle `0x67` — это Bq/m³ или CPM? Прогон на
  калиброванном фоне (наша квартира) → сравнение с показанием RadexM-app.
- [ ] Endian/scale handle `0x67`: значения 252/237 — это сырые или x10/x100?
- [ ] Battery service (`0x180F`) — НЕ найден в GATT-таблице. У MR107ion вероятно
  custom battery handle внутри FE651600 config-сервиса. Найти его через 0x2901.
- [ ] Handles config-сервиса 0x29, 0x2b, 0x2d, 0x2f, 0x33-0x3b — что это? Назвать
  через 0x2901.