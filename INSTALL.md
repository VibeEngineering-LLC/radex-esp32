# Установка прошивки Radex → ESP32 с нуля

Подробная пошаговая инструкция: что купить, что поставить, как прошить плату двумя разными способами. Не предполагает никакого предварительного опыта работы с ESPHome или Arduino.

> Краткая шпаргалка для опытных — в [`README.md`](README.md), раздел «Быстрый старт». Этот файл — для тех, у кого ничего ещё не установлено.

---

## Содержание

1. [Что в итоге получится](#что-в-итоге-получится)
2. [Требования к оборудованию](#требования-к-оборудованию)
3. [Требования к системе и софту](#требования-к-системе-и-софту)
4. [Шаг 1: Установить драйвер USB-Serial](#шаг-1-установить-драйвер-usb-serial)
5. [Шаг 2: Установить Python и ESPHome](#шаг-2-установить-python-и-esphome)
6. [Шаг 3: Получить файлы прошивки](#шаг-3-получить-файлы-прошивки)
7. [Шаг 4: Заполнить `secrets.yaml`](#шаг-4-заполнить-secretsyaml)
8. [Шаг 5: Подставить MAC Radex (опционально)](#шаг-5-подставить-mac-radex-опционально)
9. [Шаг 6: Прошить плату — выбери путь А или Б](#шаг-6-прошить-плату--выбери-путь-а-или-б)
10. [Путь А: через Claude Code](#путь-а-через-claude-code)
11. [Путь Б: через стандартный ESPHome (CLI, Web Installer, Dashboard)](#путь-б-через-стандартный-esphome)
12. [Шаг 7: Подключить плату к WiFi (captive portal)](#шаг-7-подключить-плату-к-wifi)
13. [Шаг 8: Открыть Web UI и привязать MAC прибора](#шаг-8-открыть-web-ui)
14. [Шаг 9 (опционально): Подключить к Home Assistant](#шаг-9-подключить-к-home-assistant)
15. [Шаг 10 (опционально): Народмон-выгрузка — что есть и как НЕ включить случайно](#шаг-10-народмон-выгрузка)
16. [Troubleshooting — топ-10 проблем](#troubleshooting)

---

## Что в итоге получится

После прошивки у тебя в локальной сети появляется маленький веб-сервер `http://radex-gw-s3.local/`, который:

- держит постоянную BLE-связь с детектором радона Radex MR107ion (приложение RadexM на телефоне больше не нужно — но и не мешает);
- показывает живые показания радона (`OAR_last`, среднее за час / день), температуру и влажность прямо в браузере;
- (опционально) транслирует данные в Home Assistant по нативному протоколу ESPHome с шифрованием;
- (опционально и **по умолчанию выключено**) умеет отправлять показания на народный мониторинг `narodmon.ru` четырьмя способами (HTTP GET / HTTP POST / HTTPS POST / JSON POST) — switch гарантированно держится в OFF после любого ребута / safe-mode / factory_reset / OTA.

Никакого облака. Никаких аккаунтов. Всё крутится в твоей локальной сети, исходники открыты, прошивка — твоя.

---

## Требования к оборудованию

### Детектор радона

- **Radex MR107ion** (BLE-ревизия, в эфире рекламирует имя вида `MR107ion 0214`). Прибор от Quarta-Rad / [quartarad.com](https://quartarad.com). Проверено на ревизии `0214` — должно работать на любой `MR107ion XXXX` с BLE.
- Прибор должен быть включён и заряжен.
- **Не путать с MR107 (без `ion`)** — это старая USB-ревизия без BLE, она не подойдёт.
- MAC-адрес прибора понадобится в Шаге 8. Узнать его можно так:
  - Открыть приложение **RadexM** на телефоне → подключиться к прибору → меню → информация о приборе → строка «BT address» / «MAC».
  - Или через любой BLE-сканер: на Windows — [Bluetooth LE Explorer](https://apps.microsoft.com/detail/9N0ZTKF1QD98), на Linux — `bluetoothctl scan on`, на macOS — приложение «LightBlue» из App Store, на смартфоне — [nRF Connect](https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp), искать имя `MR107ion XXXX`.
  - Или **позже через Web UI самой прошивки** — после первой загрузки в группе «Service» появится BLE-сканер, который найдёт прибор и позволит привязать его к шлюзу без правки YAML / secrets. Это рекомендуемый путь для новичков (Шаг 5 можно пропустить).

### Плата ESP32 — два варианта на выбор

> **Если сомневаешься — бери первый вариант (ESP32-S3-DevKitC-1 N16R8).** Это актуальная рекомендуемая платформа: USB-C, разъём для внешней антенны, 4 параллельных BLE-слота против 3 у классического ESP32-WROOM.

#### Вариант 1 — ESP32-S3-DevKitC-1 N16R8 ⭐ (рекомендуется)

Официальная плата от Espressif на чипе ESP32-S3 с маркировкой **N16R8** = 16 МБ Flash + 8 МБ embedded octal PSRAM. **Актуальная и рекомендованная** конфигурация для этого скилла.

- Чип: **ESP32-S3** (двухъядерный Xtensa LX7 @ 240 МГц + LP-ядро RISC-V).
- Flash: **16 МБ** (Quad SPI).
- **PSRAM: 8 МБ embedded octal-SPI**.
- **Wi-Fi 2.4 ГГц + Bluetooth 5 LE**.
- **Разъём антенны: U.FL / IPEX (IPX)** — можно подключить внешнюю штыревую антенну для большей дальности до Radex (на платке штатно стоит керамическая чип-антенна, но её можно перепаять/переключить на IPEX-разъём 0-Ω резистором).
- USB-Serial: **USB-C** разъём «UART» (через CH343 или аналог) для прошивки + второй **USB-C** «OTG» для нативного USB прямо в чип (CDC ACM, драйвер обычно встроен в ОС). Для прошивки используй UART-разъём.
- Прошивка для этой платы: **`firmware/radex_gateway_s3.yaml`** (`framework: arduino`).
- Цена: ~700–1500 ₽ на Ali (ищи именно «**ESP32-S3-DevKitC-1 N16R8**» — буква-цифра конфигурация в названии), ~2500–3500 ₽ в радиомагазинах.

> **Почему arduino, а не esp-idf на S3?** В этом скилле эмпирически выбран `framework: arduino` для S3: на Windows без MSYS2 esp-idf не собирается, а на arduino_3.0+ для S3 наблюдалась нестабильность BLE-стека в начальной фазе разработки. На arduino радон-шлюз отрабатывает стабильно ≥ суток без реконнектов (валидация v0.3.0-step3 / 2026-06-17).

#### Вариант 2 — классический ESP32-DevKitC v4 (архивная сборка для уже имеющихся плат)

«Синяя плата с серебристым экраном» на чипе ESP32-WROOM-32 (без S3, без PSRAM). Самая распространённая ESP32-плата в мире. Под этот скилл подходит **только если плата уже есть у тебя на руках** — для новой установки рекомендуется Вариант 1.

- Чип: ESP32 (двухъядерный Tensilica Xtensa LX6 @ 240 МГц).
- Flash: 4 МБ.
- RAM: 320 КБ (**без PSRAM**).
- USB-Serial мост: **CH340** или **CP2102** (зависит от партии).
- Цена: ~300–500 ₽ на Ali, ~1500 ₽ в радиомагазинах.
- Прошивка для этой платы: **`firmware/radex_gateway.yaml`** (`framework: esp-idf`, v0.3.0-step8 — последняя стабильная для классики, `bluetooth_proxy` закомментирован для изоляции стабильности; см. [`firmware/CHANGELOG.md`](firmware/CHANGELOG.md)).
- Альтернативно: **`firmware/radex_gateway_v2.yaml`** (Web Server v2, одна плоская таблица с тематическими префиксами `1.x` / `2.x` / `3.x` / `4.x` вместо двух групп v3). Выбирать, если предпочтение «видеть всё разом, без переключения между группами».

> **Не пробуй `radex_gateway_s3.yaml` на классической DevKitC** — там нет PSRAM, нет S3-чипа, набор pin-mux/USB разный. Не делай и обратное (`radex_gateway.yaml` на S3 — другой framework, разные дефолты). Каждый YAML — строго под свою плату.

### Кабель USB

- **USB → USB-C** для S3-DevKitC-1 N16R8 (современный разъём; на плате **два USB-C** — UART-разъём через CH343 для прошивки и нативный OTG-разъём в чип).
- **USB → Micro-USB** для классического DevKitC v4 (старый разъём).
- **Важно**: кабель должен быть **с данными**, не только-питание (cheap charging-only кабели не передают serial). Если у тебя в столе несколько кабелей и непонятно какой какой — пробуй кабелем от смартфона или внешнего диска, они почти всегда с данными.

### Питание после прошивки

После того как ты прошил плату, её надо где-то держать включённой 24/7. Варианты:

- USB-блок от телефона (5 V, 1 A — с запасом).
- Powerbank (ESP32 кушает ~80–120 mA в активном BLE-режиме, средний powerbank на 5000 mAh держит ~30 ч).
- Тот же ноутбук, если шлюз тебе нужен только когда ноут включён.

Размещать плату желательно в радиусе ~5–10 м от прибора Radex, на открытом воздухе (без металлической коробки между ними — заэкранирует BLE).

---

## Требования к системе и софту

### Операционная система

Работает на любой из трёх:

| ОС | Замечания |
|---|---|
| **Windows 10/11** | Самый протестированный путь (этот скилл разрабатывался на Windows 11). Нужен драйвер CH340/CP2102/CH343 (Шаг 1). |
| **Linux** (Ubuntu / Debian / Fedora / Arch) | Драйверы обычно встроены в ядро. Может потребоваться добавить юзера в группу `dialout` (`sudo usermod -aG dialout $USER` + перелогин). |
| **macOS** (Intel/Apple Silicon) | Драйверы CH340/CP2102 ставятся одной командой через Homebrew или вручную с сайта производителя. |

### Свободное место и память

- На диске: ~500 МБ под Python + ESPHome + первый билд (далее каждый билд ~150 МБ кэша, можно периодически чистить `esphome clean`).
- RAM: 4 ГБ хватит впритык; 8+ ГБ комфортно (компилятор PlatformIO при сборке S3-прошивки временно ест ~2 ГБ).

### Софт

| Компонент | Версия | Зачем |
|---|---|---|
| **Python** | 3.10 — 3.12 | ESPHome — это Python-пакет. На Python 3.13 пока есть проблемы совместимости, лучше 3.12. |
| **ESPHome** | ≥ 2025.8.0 (минимум `min_version` из YAML) — рекомендую 2026.5+ | Сама прошивка. Поставится через `pip install esphome`. |
| **Драйвер USB-Serial** | актуальный с сайта производителя моста | См. Шаг 1. |
| **Git** | любая свежая | Чтобы скачать репозиторий с прошивкой. Опционально — можно скачать ZIP с GitHub. |
| **Текстовый редактор** | любой | Notepad++, VS Code, nano, vim — для правки `secrets.yaml`. |
| **Браузер** | Chrome или Edge для Web Installer; любой для Web UI | Шаг 8 — открыть Web UI прошивки. Если планируешь Путь Б через Web Installer — нужен именно Chrome или Edge (Firefox WebSerial не поддерживает). |

---

## Шаг 1: Установить драйвер USB-Serial

Это нужно, чтобы Windows / macOS / Linux увидели плату как COM-порт. **Сначала драйвер, потом втыкай плату.**

### Какой мост на твоей плате

- **CH340 / CH341** — самый частый на дешёвых DevKitC v4 с Ali (≈80 % случаев).
- **CP2102** — на «фирменных» Espressif-платах и некоторых HW-388 клонах.
- **FTDI FT232R** — редкий, на старых партиях.
- **CH343** — на новых ESP32-S3-DevKitC-1 (USB-Serial разъём, не USB-OTG).
- **Native USB** (ESP32-S3 OTG-разъём) — драйверы встроены в ОС, ничего ставить не надо.

Если не знаешь какой у тебя — посмотри на маленький квадратный чип рядом с USB-разъёмом, на нём будет надпись.

### Windows

| Мост | Где взять драйвер |
|---|---|
| CH340/CH341 | https://www.wch-ic.com/downloads/CH341SER_ZIP.html (страница на китайском — кнопка «Download» там единственная) |
| CP2102 | https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers |
| FTDI | https://ftdichip.com/drivers/vcp-drivers/ |
| CH343 | https://www.wch-ic.com/downloads/CH343SER_ZIP.html |

Запустить установщик от администратора → Reboot → воткнуть плату. В Диспетчере устройств появится **«Порты (COM и LPT)» → USB-SERIAL CHxxx (COMx)**.

### Linux

В современных дистрибутивах драйверы CH340/CP2102/FTDI **уже встроены в ядро**. Просто воткни плату и посмотри:

```bash
dmesg | tail -20    # должна быть строка вида:
# usb 1-1: ch341-uart converter now attached to ttyUSB0
ls -l /dev/ttyUSB* /dev/ttyACM*
```

Если порт `/dev/ttyUSB0` появился, но ESPHome не может на него писать (`Permission denied`):

```bash
sudo usermod -aG dialout $USER
# Логнуться-разлогнуться. Или временный фикс: sudo chmod 666 /dev/ttyUSB0
```

### macOS

```bash
# CH340 (homebrew)
brew install --cask wch-ch34x-usb-serial-driver
# CP2102
brew install --cask silicon-labs-vcp-driver
```

После установки перезагрузка, потом плата → `ls /dev/cu.usbserial-*`.

### Как проверить, что плата увидена

**Windows (PowerShell):**

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'CH340|CP210|FTDI|CH343|USB Serial' } | Select-Object Name, DeviceID
```

Должна вернуть строку с реальным COM-портом, например `USB-SERIAL CH340 (COM7)` или `USB Serial Device (COMx)`.

> ⚠️ На некоторых машинах **COM5 — это SoundBlaster, НЕ ESP32**. Бери порт, у которого имя содержит `USB-SERIAL CH340` / `Silicon Labs CP210x` / `USB Serial Device`.

**Linux:** `ls /dev/ttyUSB* /dev/ttyACM*`

**macOS:** `ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART`

Если ничего не показывает — проблема в драйвере или в кабеле (см. Troubleshooting).

---

## Шаг 2: Установить Python и ESPHome

### Python

| ОС | Команда / ссылка |
|---|---|
| **Windows** | https://www.python.org/downloads/ — скачать **Python 3.12**, при установке поставить галку «Add Python to PATH». |
| **Linux (Ubuntu/Debian)** | `sudo apt install python3.12 python3.12-venv python3-pip` |
| **Linux (Fedora)** | `sudo dnf install python3.12 python3-pip` |
| **Linux (Arch)** | `sudo pacman -S python python-pip` |
| **macOS** | `brew install python@3.12` |

Проверить:

```bash
python --version    # должно показать Python 3.10..3.12
pip --version
```

### ESPHome

```bash
pip install esphome
# Или, если хочешь изолированно (рекомендую):
python -m venv esphome-env
# Windows: esphome-env\Scripts\activate
# Linux/macOS: source esphome-env/bin/activate
pip install esphome
```

Проверить:

```bash
esphome version
# Должно показать что-то вроде: Version: 2026.5.3
```

YAML требует **`min_version: 2025.8.0`** — любая свежее подойдёт. Рекомендую 2026.5+.

> ⚠ **Windows-нюанс**: после `pip install esphome` команда `esphome` иногда не попадает в PATH. Если `esphome version` пишет «не является внутренней или внешней командой», используй полный путь — у Python 3.12 это обычно `%LOCALAPPDATA%\Programs\Python\Python312\Scripts\esphome.exe`. Можно создать алиас в PowerShell:
>
> ```powershell
> Set-Alias esphome "$env:LOCALAPPDATA\Programs\Python\Python312\Scripts\esphome.exe"
> ```

> ⚠ **Кириллица в путях (Windows)**: если у тебя имя пользователя или путь содержат русские буквы, ESPHome может упасть с `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xd0 ...` (PlatformIO/colorama декодирует stdout в cp1251). Перед запуском в той же сессии PowerShell/CMD выставь:
>
> ```powershell
> $env:PYTHONIOENCODING = 'utf-8'
> $env:PYTHONUTF8 = '1'
> ```
>
> Этого недостаточно если в самом пути проекта есть кириллица — PlatformIO кладёт `.pioenvs/` рядом с YAML и спотыкается о Cyrillic-путь. В YAML `radex_gateway_s3.yaml` для этого предусмотрена строка `esphome.build_path: ./.esphome_build/radex-gw-s3` — она задаёт build-каталог относительно YAML, но не в `~/.esphome/build/...`. Если у тебя кириллица в имени пользователя и предыдущая мера не помогла — перенеси папку с прошивкой куда-нибудь под латинский путь (`C:\esp\radex\firmware\`).

---

## Шаг 3: Получить файлы прошивки

### Через git (рекомендую)

```bash
git clone https://github.com/Verter73/claude-skills.git
cd claude-skills/radex-esp32/firmware
ls
# radex_gateway_s3.yaml          ← АКТУАЛЬНАЯ для ESP32-S3-DevKitC-1 (arduino)
# radex_gateway_s3_baseline.yaml ← чистый baseline для S3 (smoke-тест железа)
# radex_gateway.yaml             ← v0.3.0-step8 для классической DevKitC (esp-idf)
# radex_gateway_v2.yaml          ← альтернатива на Web Server v2 (плоская таблица)
# secrets.example.yaml           ← шаблон секретов
# CHANGELOG.md                   ← история версий
# include/                       ← C-хедеры (esp_diag.h, radex_read_hook.h)
# www/                           ← CSS/JS для Web UI v2
# archive/                       ← старые YAML для отката
```

### Без git (через ZIP)

1. Открой https://github.com/Verter73/claude-skills в браузере.
2. Зелёная кнопка «Code» → «Download ZIP».
3. Распакуй, перейди в `claude-skills-master/radex-esp32/firmware/`.

> Если ты планируешь сразу шить S3 — тебе нужен **только `radex_gateway_s3.yaml`** + `secrets.yaml`. Подпапки `include/` и `www/` он не использует. Они нужны только для классической DevKitC (`radex_gateway.yaml` / `radex_gateway_v2.yaml`).

### Какую прошивку выбрать

В папке `firmware/` лежат **четыре YAML** на одной кодовой базе. Выбирай ровно один — все последующие шаги (compile / upload / Web UI / mDNS-имя) разные:

| YAML | Плата | Фреймворк | Когда выбирать | mDNS-имя |
|---|---|---|---|---|
| **`radex_gateway_s3.yaml`** ⭐ | ESP32-S3-DevKitC-1 N16R8 | `arduino` | **Рекомендованная актуальная конфигурация.** Новая установка, новая плата. Web UI v3 с тематическими группами Data/Service, `web_server.log: true` (8 МБ PSRAM — запас heap есть). | `radex-gw-s3.local` |
| `radex_gateway_s3_baseline.yaml` | ESP32-S3-DevKitC-1 N16R8 | `arduino` | **Smoke-тест железа** перед накатом полной прошивки. Минимальный набор: BLE/API/Web UI без логики опроса handle'ов. Полезен при первой прошивке новой S3-платы — если baseline работает, ставь полную; если нет, проблема в железе. | `radex-gw-s3.local` |
| `radex_gateway.yaml` | классический ESP32-DevKitC v4 (WROOM-32) | `esp-idf` | **Архивная сборка** v0.3.0-step8 для плат, которые уже есть на руках. `web_server.log: false` против `json:111`. `bluetooth_proxy: # active: true` закомментирован для изоляции BLE-стабильности. | `radex-gw.local` |
| `radex_gateway_v2.yaml` | классический ESP32-DevKitC v4 | `esp-idf` | **Альтернативный UX** на классике: Web Server v2, одна плоская таблица с префиксами `1.x`/`2.x`/`3.x`/`4.x` вместо двух групп v3. Выбирать, если предпочтение «всё разом, без переключения групп». | `radex-gw.local` |

**Решающее правило:**
- Новая плата ⇒ ESP32-S3-DevKitC-1 N16R8 ⇒ начни с **`radex_gateway_s3_baseline.yaml`** для smoke-теста, потом сразу OTA-up на **`radex_gateway_s3.yaml`**.
- Уже есть классическая DevKitC ⇒ **`radex_gateway.yaml`** (или `_v2`, если хочешь плоский UI).
- Никогда **не кросс-комбинируй** S3-YAML с классической платой и наоборот: разные framework, разные partition-таблицы, разные дефолты — плата уйдёт в boot loop или OTA не пройдёт.

---

## Шаг 4: Заполнить `secrets.yaml`

В папке `firmware/` есть файл-шаблон `secrets.example.yaml`. Скопируй его и заполни:

```bash
cp secrets.example.yaml secrets.yaml
# Windows-эквивалент: Copy-Item secrets.example.yaml secrets.yaml
```

Открой `secrets.yaml` в редакторе. Что заполнять:

| Ключ | Что туда написать |
|---|---|
| `wifi_ssid` | Имя твоей WiFi-сети **2.4 ГГц** (5 ГГц ESP32 не поддерживает!). |
| `wifi_password` | Пароль WiFi. |
| `wifi_ssid_2` | Опциональная вторая сеть (fallback / альтернативный SSID). Если не нужна — повтори значение `wifi_ssid`. |
| `wifi_password_2` | Пароль второй сети. Если не нужна — повтори `wifi_password`. |
| `ap_password` | Любой пароль ≥ 8 символов. Понадобится, если плата не сможет подключиться к домашней WiFi — поднимет свою AP, на неё ты зайдёшь с телефона с этим паролем. |
| `api_encryption_key` | Сгенерировать: `python -c "import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())"`. Этот ключ потом нужен в Home Assistant. |
| `ota_password` | Любой пароль ≥ 8 символов. Защищает от случайной перепрошивки. |
| `web_server_auth_user` | Логин для Web UI (Basic Auth). Например `admin` или `radex`. |
| `web_server_auth_pass` | Пароль для Web UI (Basic Auth). Любой пароль ≥ 8 символов. |
| `radex_mac` | MAC твоего Radex MR107ion (UPPERCASE с двоеточиями: `AA:BB:CC:DD:EE:FF`). **Можно оставить placeholder** `AA:BB:CC:DD:EE:FF` и задать MAC через Web UI после первой загрузки (см. Шаг 8). |

> ⚠ **secrets.yaml в git НЕ коммитить!** В корне репо есть `.gitignore`, который его игнорирует — не трогай.

Альтернативный быстрый способ сгенерировать все рандомные пароли сразу:

```bash
python -c "import secrets,base64; print('api_key:', base64.b64encode(secrets.token_bytes(32)).decode()); print('ota:', secrets.token_urlsafe(18)); print('web:', secrets.token_urlsafe(18)); print('ap:', secrets.token_urlsafe(12))"
```

> **HARD-правило**: на одной плате `web_server_auth_pass` фиксируется на ПЕРВОЙ заливке. Браузер Chrome кэширует Basic Auth для realm «Login Required» и после OTA с новым паролем prompt не показывает — будешь видеть 401 без формы логина. Если очень нужно сменить — сначала открой `chrome://settings/passwords` и удали запись для IP / hostname платы. Лучше: задай пароль один раз и держись.

---

## Шаг 5: Подставить MAC Radex (опционально)

> **Этот шаг можно пропустить**, если ты не знаешь MAC своего прибора. После первой загрузки в Web UI прошивки есть BLE-сканер, который найдёт `MR107ion XXXX`, и кнопка «Применить MAC и перезагрузить». MAC сохраняется в NVS, в YAML/secrets залезать не придётся.

Если хочешь подставить MAC заранее (например, переиспользуешь старую конфигурацию):

В YAML-файле (`radex_gateway_s3.yaml` или `radex_gateway.yaml`, в зависимости от твоей платы) есть строка:

```yaml
substitutions:
  ...
  radex_mac_str: "AA:BB:CC:DD:EE:FF"
```

Заменить `AA:BB:CC:DD:EE:FF` на свой реальный MAC (тот же, что в `secrets.yaml::radex_mac`). Эта substitution используется в лямбде для RSSI sensor'а — она сравнивает строку, поэтому нужна именно текстом, не через `!secret`.

> **Где взять MAC прямо сейчас:** открой приложение **RadexM** на смартфоне → подключись к прибору → информация о приборе → «BT address». Или любым BLE-сканером (nRF Connect / LightBlue / Bluetooth LE Explorer) — искать имя `MR107ion XXXX`, где `XXXX` — последние 4 цифры серийного номера прибора.

---

## Шаг 6: Прошить плату — выбери путь А или Б

Теперь главная развилка:

- **Путь А** — попросить AI-агента (Claude Code или аналог) пройти все шаги за тебя. Удобно если ты впервые слышишь слова «ESPHome» и «COM-порт». Требует подписку Anthropic (или $5 на API-баланс) — см. ниже про бесплатную опцию.
- **Путь Б** — сделать руками через стандартный ESPHome. Бесплатно. Три варианта: CLI, ESPHome Dashboard, ESPHome Web Installer (через Chrome).

> Можно начать с А, если что-то пойдёт не так — переключиться на Б. И наоборот.

---

## Путь А: через Claude Code

### Что такое Claude Code

Claude Code — это CLI-агент от Anthropic. Запускаешь его в терминале, он умеет читать файлы, запускать команды (с твоим подтверждением), задавать вопросы. Для прошивки удобно: ты говоришь ему «прошей плату», он сам определяет COM-порт, компилирует, заливает, открывает Web UI.

### Установка Claude Code

```bash
# Требует Node.js ≥ 18. Поставить можно отсюда: https://nodejs.org/
npm install -g @anthropic-ai/claude-code

# Проверить
claude --version
```

Запустить в папке с прошивкой:

```bash
cd claude-skills/radex-esp32/firmware
claude
```

При первом запуске спросит логин — войти через **claude.ai** или ввести API-ключ с https://console.anthropic.com/.

### Доступ — что доступно бесплатно, а что нет

| Способ | Бесплатно? | Что можно |
|---|---|---|
| **Claude Code CLI с подпиской Claude Pro/Max** ($20/$100/$200 в месяц) | Нет, но включено в подписку | Полноценный агент с шагами прошивки. |
| **Claude Code CLI с API-ключом** (pay-as-you-go) | Нет; примерная стоимость одной сессии прошивки — $0.50–2 | То же. |
| **Claude.ai (веб) — бесплатный тариф** | **Да**, с дневным лимитом | Можно зайти на claude.ai, спросить совет, скопировать команды в свой терминал и выполнить вручную. **Веб-Claude не имеет доступа к твоему COM-порту**, прошивать должен ты сам. |
| **Cursor IDE с бесплатным лимитом Claude** | Да, ~2000 запросов в месяц на старте | Cursor — IDE с встроенным AI-чатом. Бесплатного лимита хватит на десятки прошивок. **Тоже не имеет прямого доступа к COM**, но может терминальные команды предложить. |
| **Aider / Continue.dev + локальная Ollama** | Полностью бесплатно | Open-source агенты, цепляются к локальной модели (например `qwen3-coder:30b`). Для шага прошивки — ОК. |

**Рекомендация**: если хочется бесплатно — открыть https://claude.ai (без подписки), вставить промпт ниже, скопировать выданные команды в свой терминал и выполнить руками. AI не запустит `esphome run` за тебя, но даст пошаговый план и поможет с ошибками. Если готов платить $20/мес или $5 на API — установить Claude Code CLI, он сделает всё сам.

### Готовый промпт для Claude Code (или для веб-Claude)

Скопируй текст ниже и вставь в чат с Claude (CLI или веб). На веб-Claude он даст пошаговый план; в CLI — реально пройдёт по шагам и предложит запустить команды.

```
У меня есть детектор радона Radex MR107ion (BLE-ревизия, в эфире рекламирует
имя MR107ion XXXX) и плата ESP32 — рекомендуемая ESP32-S3-DevKitC-1 N16R8
(16 MB Flash + 8 MB embedded octal PSRAM, U.FL/IPEX, USB-C) или, как
альтернатива, классический ESP32-DevKitC v4 (если уже есть на руках).
Я хочу прошить плату прошивкой из репозитория
https://github.com/Verter73/claude-skills (папка radex-esp32/firmware/).

Сделай по шагам, на русском, нумеруя:

1. Спроси у меня, какая у меня плата:
   - ⭐ ESP32-S3-DevKitC-1 N16R8 (чёрная плата с двумя USB-C, разъём U.FL/IPEX
     для антенны) — РЕКОМЕНДУЕТСЯ →
     прошивка radex_gateway_s3.yaml (framework: arduino);
   - классический ESP32-DevKitC v4 (синяя плата с micro-USB, без PSRAM) →
     прошивка radex_gateway.yaml (framework: esp-idf, v0.3.0-step8 — последняя
     стабильная сборка для классики).
   Запомни выбор и используй везде дальше. Если оператор не уверен — рекомендуй S3 N16R8.

2. Проверь, что репо уже клонирован (cwd — папка firmware/). Если нет — клонируй:
   git clone https://github.com/Verter73/claude-skills.git
   cd claude-skills/radex-esp32/firmware

3. Проверь установлен ли ESPHome:
   esphome version
   Если нет — pip install esphome (требует Python 3.10..3.12, ESPHome ≥ 2025.8.0).

4. Открой secrets.example.yaml и помоги мне заполнить secrets.yaml:
   - спроси SSID и пароль моей домашней WiFi 2.4 ГГц
     (если у меня только 5 ГГц — предупреди что ESP32 не поддерживает);
   - спроси про вторую сеть (wifi_ssid_2/wifi_password_2). Если не нужна —
     дублируй значения первой;
   - сгенерируй api_encryption_key через
     python -c "import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())"
   - сгенерируй ota_password, web_server_auth_pass и ap_password через
     python -c "import secrets; print(secrets.token_urlsafe(18))"
   - спроси MAC моего Radex (формат UPPERCASE с двоеточиями); если не знаю —
     предложи открыть приложение RadexM → информация о приборе → BT address.
     Если совсем не хочу искать — оставь placeholder AA:BB:CC:DD:EE:FF в
     secrets.yaml и в substitutions.radex_mac_str, MAC задам через Web UI после
     первой загрузки (там есть BLE-сканер и кнопка «Применить MAC и перезагрузить»).
   Запиши всё в secrets.yaml. Покажи мне финальный файл (пароли можешь замаскировать).

5. Если MAC известен — подставь его в substitutions.radex_mac_str в выбранном YAML.

6. Определи COM-порт платы:
   - Windows (PowerShell):
     Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'CH340|CP210|FTDI|CH343|USB Serial' }
   - Linux: ls /dev/ttyUSB* /dev/ttyACM*
   - macOS: ls /dev/cu.usbserial-*
   Покажи мне список, попроси подтвердить нужный порт (на классическом DevKitC
   это обычно единственный CH340/CP2102; на S3 могут быть два — нужен тот,
   что подписан CH343 или CDC ACM).

7. Скомпилируй прошивку (Windows-нюанс — PYTHONIOENCODING + PYTHONUTF8 если в
   пути или имени пользователя кириллица):
   $env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
   esphome compile <выбранный-yaml>
   Если компиляция падает — расскажи мне почему и как починить. Не запускай
   upload пока компиляция не прошла.

8. Прошей:
   esphome upload <выбранный-yaml> --device <COM-порт>
   Если автоматическое срабатывание BOOT не работает (плата висит на «Connecting...
   ___...») — попроси меня зажать кнопку BOOT, нажать RESET, отпустить BOOT.

9. После прошивки — расскажи что плата сейчас сделала:
   - перезагрузилась через RTS;
   - попыталась подключиться к WiFi из secrets.yaml;
   - если не вышло — подняла AP «radex-gw-s3 Fallback» (для S3) или
     «radex-gw Fallback» (для классики).
   Объясни мне, как зайти на captive portal с телефона если нужно.

10. После того как плата в сети — открой
    http://radex-gw-s3.local/ (S3) или http://radex-gw.local/ (классика)
    в браузере. Запроси Basic Auth: username и пароль из secrets.yaml
    (web_server_auth_user / web_server_auth_pass). Помоги мне найти группу
    «Service» (на S3 — sg_service), запустить BLE-сканер, найти строку
    `MR107ion XXXX`, скопировать MAC, вставить в поле «MAC Radex» и нажать
    «Применить MAC и перезагрузить». После рестарта в группе «Data»
    (sg_data) должны побежать радон / температура / влажность.

11. Народмон-выгрузку НЕ ВКЛЮЧАЙ без явного запроса оператора. Switch
    «Выгружать на Народмон» в прошивке создан с restore_mode: ALWAYS_OFF —
    после ребута / OTA / factory_reset гарантированно вернётся в OFF.
    Если оператор спрашивает «как включить» — расскажи, но не нажимай сам.

12. Если что-то идёт не так на любом шаге — сначала задай уточняющий вопрос,
    не делай destructive-операций без подтверждения (не удаляй файлы, не
    перезапускай безусловно).

Действуй. Начни с шага 1 — спроси какая у меня плата.
```

### Если ты на бесплатном веб-Claude (claude.ai без подписки)

Вставь тот же промпт, но добавь сверху:

> **Я открыл тебя в браузере (claude.ai, бесплатный тариф). У тебя нет доступа к моему компьютеру. Каждую команду показывай мне в код-блоке, я скопирую и выполню сам, потом покажу тебе результат.**

После этого работа идёт диалогом: Claude выдаёт команду — ты её копируешь — выполняешь — копируешь вывод обратно в чат. Медленнее, чем CLI-агент, но бесплатно. Для одной прошивки сессия занимает ~30 минут, лимит свободного тарифа хватит.

---

## Путь Б: через стандартный ESPHome

### Б.1 — ESPHome CLI (самый простой, рекомендую для второй прошивки)

В папке `firmware/`:

```bash
# Только проверка YAML (без компиляции):
esphome config radex_gateway_s3.yaml

# Скомпилировать (займёт 3-5 минут первый раз, потом ~1 мин с кэшем):
esphome compile radex_gateway_s3.yaml

# Прошить через USB:
#   Windows:
esphome upload radex_gateway_s3.yaml --device COM7
#   Linux:
esphome upload radex_gateway_s3.yaml --device /dev/ttyUSB0
#   macOS:
esphome upload radex_gateway_s3.yaml --device /dev/cu.usbserial-0001

# Или одной командой (compile + upload):
esphome run radex_gateway_s3.yaml --device COM7
```

После первой прошивки плата получит OTA-обновления **по воздуху**:

```bash
esphome run radex_gateway_s3.yaml --device radex-gw-s3.local
# или по IP:
esphome run radex_gateway_s3.yaml --device 192.168.1.42
```

**Если у тебя классический DevKitC** — заменяй имя YAML и hostname:

```bash
esphome run radex_gateway.yaml --device COM7
esphome run radex_gateway.yaml --device radex-gw.local   # после первой прошивки
```

Логи (UART, **не** для arduino-S3 — там DTR/RTS дёргает RESET и обрывает BLE):

```bash
# По воздуху (безопасно):
esphome logs radex_gateway_s3.yaml --device radex-gw-s3.local

# Через USB — НЕ ИСПОЛЬЗУЙ на этой прошивке!
# esphome logs --device COM7  ← DTR/RTS периодически дёргает RESET, плата перезагружается
# Если нужен UART — используй PowerShell + System.IO.Ports.SerialPort с DTR=OFF/RTS=OFF
# или putty с этими же настройками.
```

> Запрет `esphome logs --device COMx` — HARD-правило проекта Radex. Каждое подключение через esphome logs ребутит плату через DTR/RTS, что обрывает BLE-сессию с Radex и сбивает счётчики реконнектов. Используй OTA-логгер (`--device hostname.local`) или сторонний serial-просмотрщик с отключёнными DTR/RTS.

### Б.2 — ESPHome Dashboard (через Home Assistant Add-on)

Если у тебя Home Assistant — поставь Add-on «ESPHome Builder», открой dashboard, кнопка «New Device» → импортируй YAML вручную. Это удобно если планируешь подключать к HA сразу.

Минусы: только для тех, у кого уже есть HA. Для standalone Web UI избыточно.

### Б.3 — ESPHome Web Installer (через Chrome, без локального Python)

> Подходит только если ты сам собираешь `.bin` локально, либо если тебе кто-то прислал готовый `.bin`. **В этом репозитории `.bin`-файлов нет** (политика безопасности — не публикуем бинарники с зашитыми чужими секретами).

Чтобы воспользоваться Web Installer, надо сначала собрать `.bin` локально через `esphome compile`. После компиляции ESPHome кладёт собранный образ сюда:

- **Windows / Linux / macOS** (стандарт): `~/.esphome/build/radex-gw-s3/.pioenvs/radex-gw-s3/firmware.factory.bin`
- **Если в YAML стоит `esphome.build_path: ./.esphome_build/radex-gw-s3`** (workaround для кириллицы в пути): `<папка-с-yaml>/.esphome_build/radex-gw-s3/.pioenvs/radex-gw-s3/firmware.factory.bin`

(для классического DevKitC — `radex-gw` вместо `radex-gw-s3`)

Дальше:

1. Открой https://web.esphome.io в **Chrome** или **Edge** (Firefox WebSerial не поддерживает).
2. Воткни плату по USB.
3. Кнопка «Connect» → выбери COM-порт.
4. «Choose File» → укажи `firmware.factory.bin`.
5. «Install».

Преимущество перед CLI: не надо больше ничего запускать на машине после первой компиляции — плату можно прошить с любого Windows-компа с Chrome. Удобно если делишься прошивкой с другом.

---

## Шаг 7: Подключить плату к WiFi

После прошивки плата перезагрузится. Дальше два сценария:

### Сценарий 1 — секреты в `secrets.yaml` правильные

Плата увидит твою WiFi, подключится, получит IP по DHCP. Через ~15 секунд после `Hard reset RTS` она уже в сети. Пингани:

```bash
ping radex-gw-s3.local
# или (для классической DevKitC):
ping radex-gw.local
```

Если ответ есть — переходи к Шагу 8.

### Сценарий 2 — `secrets.yaml` пустой, с опечаткой, или обе сети из `wifi.networks` недоступны

Плата не подключится к домашней WiFi за 60 с и **поднимет свою AP**:

- SSID: `radex-gw-s3 Fallback` (S3) или `radex-gw Fallback` (классическая DevKitC).
- Пароль: `ap_password` из твоего `secrets.yaml`.

С телефона/ноута подключись к этой AP — браузер автоматически откроет **captive portal** на `192.168.4.1`. Введи правильные SSID + пароль домашней WiFi → плата сохранит их в NVS, перезагрузится, в этот раз подключится.

> Если captive portal не открылся автоматически — открой в браузере явно `http://192.168.4.1/`.

> Если ты на Windows и из домашней сети `radex-gw-s3.local` не пингуется — почти всегда дело в том, что mDNS заблокирован файрволом. Проверь IP через роутер (admin-панель → DHCP clients) и пингуй по IP. Имя `radex-gw-s3` ищи в списке клиентов роутера.

---

## Шаг 8: Открыть Web UI и привязать MAC прибора

```
http://radex-gw-s3.local/        (S3)
http://radex-gw.local/           (классическая DevKitC)
```

Браузер спросит **Basic Auth** — введи `web_server_auth_user` / `web_server_auth_pass` из своего `secrets.yaml`.

Что ты должен увидеть на свежей прошивке (MAC прибора ещё не привязан или placeholder):

- **Группа «Data»** (`sg_data`): пока пусто или показывает NaN — потому что шлюз ещё не подключился к Radex по BLE.
- **Группа «Service»** (`sg_service`):
  - BLE-сканер (кнопка «Start BLE scan» или аналог).
  - Поле «MAC Radex» — туда вставлять найденный MAC.
  - Кнопка «Применить MAC и перезагрузить».
  - Диагностика WiFi: сигнал / SSID / IP / MAC шлюза.
  - Народмон-инфраструктура (switch OFF, select «Способ отправки», 3 поля имён метрик RR1/T1/H1, кнопка «Отправить на Народмон сейчас»).
  - Кнопки Reboot / Safe Mode / Factory reset / Переподключиться к Radex / Сбросить счётчики BLE.

### Найти MAC через Web UI BLE-сканер

1. Зайди в группу «Service».
2. Запусти BLE-скан.
3. Через 10–20 секунд в списке появится строка вида `MR107ion 0214 │ AA:BB:CC:DD:EE:FF │ -65 dBm`.
4. Скопируй MAC, вставь в поле «MAC Radex», нажми «Применить MAC и перезагрузить».
5. После рестарта в группе «Data» побегут:
   - Радон последний (Bq/m³) — обновляется ~раз в 17 с (round-robin READ-poll 4 handle'ов).
   - Радон среднее (Bq/m³) — скользящее окно час + день.
   - Температура (°C).
   - Влажность (%).

Если сканер ничего не находит — открой раздел Troubleshooting (топик 6).

### Что значит «BLE подключён = ON / OFF»

- **ON** — шлюз держит постоянный BLE-коннект к прибору и поллит handle'ы.
- **OFF** — прибор не отвечает. Самые частые причины:
  - **Приложение RadexM на телефоне всё ещё держит коннект к прибору** (MR107ion — single-central, или приложение, или шлюз; одновременно нельзя). Закрой RadexM полностью.
  - MAC в поле «MAC Radex» — с опечаткой. Перепроверь.
  - Прибор выключен или села батарейка.
  - Прибор далеко / много стен.

---

## Шаг 9: Подключить к Home Assistant

(Опционально. Если HA у тебя нет — пропускай.)

В HA → Settings → Devices & Services → Add Integration → **ESPHome**:

| Поле | Значение |
|---|---|
| Host | `radex-gw-s3.local` (или `radex-gw.local`, или IP) |
| Port | `6053` (не менять) |
| Encryption key | значение `api_encryption_key` из твоего `secrets.yaml` |

HA подхватит все сенсоры автоматически (радон последний / среднее за час / среднее за день, температура, влажность, BLE-счётчики, WiFi-метрики, uptime). Если хочешь — настрой автоматизации (например, push в Telegram при превышении 300 Bq/m³, или ежедневный отчёт за сутки).

---

## Шаг 10: Народмон-выгрузка

(Опционально. По умолчанию **выключено** через `restore_mode: ALWAYS_OFF` — HARD-правило проекта.)

Прошивка содержит инфраструктуру для отправки показаний на **народный мониторинг** [`narodmon.ru`](https://narodmon.ru) — но **switch всегда возвращается в OFF** после любого ребута / safe-mode / factory_reset / OTA. Чтобы реально пошла отправка, switch нужно явно перевести в ON через Web UI или через HA — и это надо делать осознанно, после каждой перезагрузки.

### Что есть в Web UI (группа «Service»)

| Сущность | Что делает |
|---|---|
| Switch **«Выгружать на Народмон»** | главный выключатель отправки; **`restore_mode: ALWAYS_OFF`** — после reboot гарантированно OFF |
| Select **«Способ отправки»** | 4 транспорта: `HTTP GET` / `HTTP POST` / `HTTPS POST` / `JSON POST`. Дефолт — `HTTP GET` |
| Поля **«Имя метрики»** RR1 / T1 / H1 | имена метрик на стороне Народмона (по умолчанию RR1 = радон, T1 = температура, H1 = влажность) |
| Кнопка **«Отправить на Народмон сейчас»** | ручной триггер; работает только при включённом switch |
| Авто-интервал | 600 с (5 мин — минимум на стороне Народмона; короче → бан IP на час) |

### Почему по умолчанию OFF

Народмон — публичный сервис: твой MAC и привязка к локации становятся видимы любому. Это **сознательное решение** оператора скилла — никаких сюрпризов после ребута. Если ты хочешь выгружать — это твой выбор; включай вручную, перечитав политику Народмона.

### Как реально включить (если очень нужно)

1. Зарегистрировать MAC шлюза в [личном кабинете Народмона](https://narodmon.ru/login).
2. В Web UI поставить switch «Выгружать на Народмон» = ON. Подождать 5 минут — должна прилететь первая отправка.
3. Проверить в Народмоне: страница датчика → последний replied value.
4. Если выгрузка работает — оставить switch ON; **после первого reboot switch вернётся в OFF, придётся включать заново**. Это HARD-правило, не баг.

> **Не пробуй ставить `restore_mode` иной, чем `ALWAYS_OFF`** — нарушение приведёт к сюрпризу: после ребута / OTA switch внезапно окажется ON и пойдёт выгрузка. Этот скилл такого не допускает.

---

## Troubleshooting

### 1. Плата не определяется как COM-порт

- Кабель only-charging? Замени на кабель с данными.
- Драйвер не поставился (Windows)? Открой Диспетчер устройств — если есть «Неизвестное устройство» с жёлтым треугольником — кликни ПКМ → Обновить драйвер → автоматический поиск. Если не помогло — ставь драйвер с сайта производителя (Шаг 1).
- На S3: USB-C разъём с одной стороны платы — это USB-OTG в чип (CDC ACM, нужен другой драйвер); с другой стороны — UART через CH343. Попробуй второй разъём. Имя в Win11 без драйвера CH343 — `USB Serial Device (COMx)` для нативного USB-OTG.

### 2. `esphome compile` падает с `UnicodeDecodeError`

- Кириллица в имени пользователя / пути → выстави в текущей сессии:
  ```powershell
  $env:PYTHONIOENCODING = 'utf-8'
  $env:PYTHONUTF8 = '1'
  ```
- Если строка с ошибкой содержит путь типа `...\Рабочая папка ИИ\...` — кириллица в пути проекта, а не только в имени пользователя. PlatformIO кладёт `.pioenvs/` рядом с YAML и спотыкается. В `radex_gateway_s3.yaml` для этого предусмотрена строка `esphome.build_path: ./.esphome_build/radex-gw-s3`. Если эта мера не помогла — перенеси папку с прошивкой куда-нибудь под латинский путь (`C:\esp\radex\firmware\`).
- Имя пользователя на латинице, а ошибка всё равно? Проверь что в `secrets.yaml` нет кириллицы в значениях (кириллица в WiFi-пароле — это норма, в `wifi_ssid` — тоже норма, но в комментариях бывают edge cases на старых Python).

### 3. `esphome upload`: «Failed to connect to ESP32: No serial data received»

- Это автоматическое срабатывание BOOT не сработало. Зажми кнопку **BOOT** на плате, нажми **RESET** (или EN), отпусти **BOOT** — и быстро запусти `esphome upload` снова. На некоторых клонах нет кнопки BOOT — нужно замкнуть GPIO0 на GND рукой во время старта.
- На S3-DevKitC-1 есть штатные кнопки BOOT и RESET с двух сторон от антенны. На классическом DevKitC v4 — две кнопки рядом с micro-USB.

### 4. После прошивки плата ребутится в цикле (`rst:0x1 (POWERON)` каждые 5 с)

- Скорее всего — несовместимая структура NVS (например, после смены framework arduino ↔ esp-idf). Сделай полное стирание flash перед заливкой:
  ```bash
  esptool.py --port COM7 erase_flash
  # потом снова esphome upload
  ```
- Или: `min_free_heap` упал в 0 — `web_server.log: true` на DevKitC без PSRAM (см. CHANGELOG, INC v0.3.0-step5b). На классическом DevKitC YAML `radex_gateway.yaml` ставит `log: false` по умолчанию. На S3 с 8 МБ PSRAM `log: true` оставлен сознательно — если на S3 пошли ребуты, поменяй на `log: false` без переспроса.

### 5. Web UI открывается, но карточек нет / пустые

- Возможно это `INC-10` — Windows-side HTTP-proxy (XRay, VPN) держит keep-alive на ESP32, исчерпывает LWIP-пул. Открой в Windows: Настройки → Сеть и интернет → Прокси → отметить «Не использовать для локальных адресов», в список добавить `192.168.*;10.*;172.16.*;localhost;127.*;*.local`. Откати браузер.
- Или: ESPHome-кэш битый. Останови сервисы, удали `~/.esphome/build/radex-gw-s3/` (или папку из `esphome.build_path`) и пересобери.

### 6. `BLE: Radex подключён = OFF` после привязки MAC

- **Самая частая причина**: приложение **RadexM** на телефоне всё ещё держит коннект к прибору. **Single-central**: или RadexM, или шлюз — одновременно нельзя. Закрой RadexM полностью (на iOS — свайпом из app-switcher, на Android — Force Stop в настройках приложений). Через 10–30 секунд шлюз подхватит прибор.
- MAC в поле «MAC Radex» — с опечаткой. Перепроверь, должен быть точно как в приборе, UPPERCASE с двоеточиями.
- Прибор разряжен или выключен (BLE-маяк работает только при включённом приборе).
- Расстояние > 10 м или стена через две комнаты — попробуй приблизить шлюз к прибору на эпоху проверки.
- В Web UI группа «Service» → кнопка «Переподключиться к Radex» — форсирует BLE-disconnect и новый коннект (если шлюз застрял в подвисшем состоянии).

### 7. `WiFi: Auth Expired` каждые несколько секунд

- 2.4 ГГц-only? Проверь что точка доступа отдаёт 2.4 ГГц (некоторые роутеры по умолчанию объединяют 2.4+5 в один SSID, отключают 2.4 при «Smart Connect»).
- Слишком далеко / много стен? RSSI < −80 dBm → handshake не успевает. Приблизь.
- Пароль с кириллицей? ESPHome их понимает, но если WiFi-роутер использует другую кодировку (cp1251 vs UTF-8) — могут быть нюансы. Перевыставь пароль на латинице для проверки.
- На S3 + arduino пока не наблюдалось систематически. Если повторяется — открывай issue с UART-логом.

### 8. `json:111: JSON document overflow` в логах + пустой Web UI

- Это исторический INC v0.3.0-broken-no-log-false. Корень — `web_server.log: true` на DevKitC без PSRAM при открытом браузере + http_request к Народмону. На классической DevKitC YAML `radex_gateway.yaml` ставит `log: false` по умолчанию.
- Если ты сам редактировал YAML и включил `log: true` на DevKitC без PSRAM — выключи.
- На S3-baseline `log: true` оставлен сознательно (8 МБ PSRAM, запас heap). Если на S3 всё равно json:111 — это новый кейс, открывай issue.

### 9. OTA не работает (после смены `web_server_auth_pass` плата не пускает)

- Это HARD-правило 2026-06-17: `web_server.auth.password` фиксируется на первой заливке. Браузер Chrome кэширует Basic Auth для realm «Login Required» и после OTA с новым паролем prompt не показывает. Лечение:
  - Открыть в Chrome `chrome://settings/passwords` → удалить запись для IP / hostname платы.
  - Или открыть URL с userinfo: `http://admin:новый_пароль@radex-gw-s3.local/` — Chrome обновит cached auth.
- Лучше: **не меняй web_server_auth_pass между прошивками одной платы.** Один раз задай, держись.

### 10. После правки только `secrets.yaml` плата шьётся со старыми секретами

- Это известный кэш-bug ESPHome: после правки **только** `secrets.yaml` (без изменения YAML) `esphome upload` может залить **старый кэшированный `firmware.bin`** без пересборки. Лечение:
  ```bash
  esphome clean radex_gateway_s3.yaml
  esphome compile radex_gateway_s3.yaml
  esphome upload radex_gateway_s3.yaml --device COM7
  ```
- Эту команду делай **всегда** после правки `secrets.yaml`. Это безопасно: `esphome clean` удаляет только build-каталог, не трогает YAML и NVS на плате.

### 11. Команды Claude Code не запускаются с правильной кодировкой (Windows)

- Перед запуском в той же сессии PowerShell:
  ```powershell
  $env:PYTHONIOENCODING = 'utf-8'
  $env:PYTHONUTF8 = '1'
  ```
- В CMD:
  ```cmd
  set PYTHONIOENCODING=utf-8
  set PYTHONUTF8=1
  chcp 65001
  ```
- Эти настройки **не сохраняются** между сессиями терминала — каждый раз делай заново. Если надоело — поставь их в System Environment Variables.

---

## Что дальше

После того как Web UI работает:

- **Прочитай [`README.md`](README.md)** — там подробности про BLE-протокол MR107ion (READ-poll, не Notify), карту GATT с handle 0x0049 (OAR_last), известные ограничения.
- **Прочитай [`SKILL.md`](SKILL.md)** — там обзор всей линейки приборов Quarta-Rad / Radex и методология RE нового прибора (если у тебя появится RD1212-BT или Radex One и захочется добавить).
- **Прочитай [`firmware/CHANGELOG.md`](firmware/CHANGELOG.md)** — там история всех версий с описанием инцидентов (`json:111`, lit fix, Народмон-инкременты).
- **Подключи к Home Assistant** (Шаг 9) — для долгосрочного хранения графиков. Web UI ESPHome хранит историю только с последнего ребута (~1 час).
- **Обновляй прошивку по воздуху** — каждое обновление этого скилла можно прошить через `esphome run radex_gateway_s3.yaml --device radex-gw-s3.local`. Никаких USB-кабелей после первой прошивки больше не нужно.
- **Не включай Народмон-выгрузку** без понимания политики Народмона. В YAML есть инфраструктура (switch «Выгружать на Народмон»), но он **по умолчанию выключен** через `restore_mode: ALWAYS_OFF` — даже после ребута / factory_reset / OTA вернётся в OFF. Это HARD-safety: «Народмон» — публичный сервис, неверная конфигурация скомпрометирует твой MAC и привязку к локации.

---

## Лицензия и обратная связь

MIT, всё открыто, форки приветствуются.

Баги / pull requests / новые модели Radex / Quarta-Rad: https://github.com/Verter73/claude-skills/issues
