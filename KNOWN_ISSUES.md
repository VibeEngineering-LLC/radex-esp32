# Known Issues & Hardware Compatibility — radex-esp32

Краткий справочник: какие платы мы реально проверили на Radex MR107ion BLE-шлюзе, какие проблемы клиенты ловят чаще всего, и что с этим делать. Длинные технические разборы инцидентов с прошлой разработки — в [README.md](README.md), раздел «Стабильность платформ».

---

## 1. Матрица совместимости плат (hardware compatibility)

> **Дисклеймер.** Работа подтверждена только на платах со статусом ✅. Для всех остальных — **гарантий нет**. Если попробовал и получилось/не получилось — открой [issue](https://github.com/VibeEngineering-LLC/radex-esp32/issues), добавим в матрицу.

### ✅ Протестированы и рекомендованы

| Плата | Чип | Flash | PSRAM | Антенна | YAML | Заметки |
|---|---|---|---|---|---|---|
| **ESP32-S3-DevKitC-1 N16R8** ⭐ | ESP32-S3 dual-core 240 MHz | 16 MB | 8 MB embedded octal | PCB + **U.FL/IPEX** | `firmware/radex_gateway_s3.yaml` (arduino) | **Рекомендованная плата.** Стабильна на arduino, PSRAM позволяет держать Web UI с `log: true`, U.FL для внешней антенны. USB-C. На smoke-тест новой платы — `firmware/radex_gateway_s3_baseline.yaml`. |
| **ESP32-DevKitC v4 (WROOM-32 / WROVER)** | ESP32 classic dual-core 240 MHz | 4 MB | — | PCB | `firmware/radex_gateway.yaml` (esp-idf v0.3.0-step8) или `firmware/radex_gateway_v2.yaml` (Web UI v2) | ⚠ Работает в **узкой конфигурации**: esp-idf + `web_server.log: false` + `bluetooth_proxy: # active: true` (закомментирован). Любая попытка вернуть `log: true` или включить bt-proxy → `json:111`, OOM. |

### ❌ НЕ работает / не подходит

| Плата | Причина |
|---|---|
| **ESP32-C3 SuperMini** | См. **INC-C3-001** ниже. Готового C3-YAML в скилле нет; попытка прошить наш S3-вариант → `OSError: [Errno 28] No space left on device`. |
| **ESP32-S2** | Нет BLE — физически не может быть BLE-шлюзом к Radex. |
| **ESP8266 (любой)** | Нет BLE. |

### ⚠ Не протестированы — на свой риск

| Плата | Ожидание |
|---|---|
| **XIAO ESP32-C3** (Seeed) | Для Radex не пробовали. NimBLE BLE 5.0 есть, но потребуется отдельный YAML — текущие наши под C3 не пойдут (см. INC-C3-001). |
| **ESP32-C6 / H2** | BLE 5.0 LE поддерживается, NimBLE OK, но YAML под них в скилле не написан. |
| **ESP32-S3-DevKitC-1 N8R2 / N4R2** | Меньше Flash/PSRAM. `radex_gateway_s3.yaml` ориентирован на N16R8; на меньших ревизиях нужно править `flash_size`, `partition` и `psram: mode`. |
| **«Голый» ESP32-S3 без PSRAM или с QSPI PSRAM** | Меняй `psram.mode: octal` → `quad` или убирай блок целиком; `web_server.log: false` без переспроса. Лучше взять оригинальную N16R8 от Espressif. |

---

## 2. Рекомендации по выбору железа

1. **Покупаешь новую плату под продакшен**: **ESP32-S3-DevKitC-1 N16R8** (Espressif оригинал). Стабильна, есть запас по памяти под Web UI v3 + sliding-window средние, U.FL для внешней BLE-антенны, USB-C. Прошивка — `firmware/radex_gateway_s3.yaml`, smoke-тест — `firmware/radex_gateway_s3_baseline.yaml`.
2. **Уже есть классический ESP32-DevKitC v4**: используй `firmware/radex_gateway.yaml` (esp-idf, v0.3.0-step8, `log: false`, bt-proxy закомментирован) или `firmware/radex_gateway_v2.yaml` (Web UI v2). Любое отклонение от этой конфигурации (включить лог, вернуть bt-proxy) → json:111.
3. **Что не брать**:
   - ESP32-C3 SuperMini (плохая PCB-антенна у части партий + готового YAML нет).
   - ESP32-S2 / ESP8266 (нет BLE).
   - Безымянные клоны S3 без PSRAM или с QSPI PSRAM (поломанный baseline).

---

## 3. Инциденты

### INC-C3-001 — `OSError: [Errno 28] No space left on device` при прошивке ESP32-C3 SuperMini

- **Дата:** 2026-06-19
- **Скилл:** radex-esp32 (симптом идентичен в atomfast-esp32)
- **Severity:** блокирует прошивку

**Симптом.** При попытке `esphome run --device COMx radex_gateway_s3.yaml` (или другой YAML с разметкой на 16 MB Flash) на плату **ESP32-C3 SuperMini** в логе появляется:

```
OSError: [Errno 28] No space left on device
```

Текст звучит как «кончилось место на диске PC», но на host-машине места много (десятки/сотни GB свободно). Ошибка приходит из esptool / arduino-builder / esp-idf flash tool.

**Корень.**

1. `radex_gateway_s3.yaml` рассчитан на **ESP32-S3-DevKitC-1 N16R8** — Flash 16 MB. Параметры `esp32.flash_size` и таблица партиций в YAML соответствуют 16 MB.
2. **ESP32-C3 SuperMini** имеет **4 MB Flash**.
3. При записи esptool проверяет `partition_table.end_offset > chip_flash_size` → `ENOSPC` (`Errno 28`).

Дополнительные факторы:
- `radex_gateway.yaml` / `radex_gateway_v2.yaml` (классический DevKitC) тоже не подойдут для C3 — они под `board: esp32dev` и **Bluedroid** BLE-стек, а C3 поддерживает **только NimBLE**. Компиляция упадёт ещё до flash на этапе линковки.
- Web UI v3 с sliding-window средними hour/day + 4-handle round-robin BLE poll на 400 KB SRAM C3 (без PSRAM) — даже если соберётся, рискует OOM на F5.

**Решение.**

- **Не пытаться прошить существующие YAML на ESP32-C3** — ни `_s3.yaml`, ни классику. Они не для C3.
- Для Radex→HA шлюза на C3 нужен **отдельный YAML** под `board: esp32-c3-devkitm-1`, `framework: arduino`, partition `min_spiffs.csv`, NimBLE-стек, упрощённый Web UI без `log: true`. На момент 2026-06-19 такого YAML в репо нет.
- **Рабочая альтернатива:** перейти на **ESP32-S3-DevKitC-1 N16R8** и использовать готовый `radex_gateway_s3.yaml`.

**Профилактика.** Перед прошивкой незнакомой платы проверь физический Flash:

```bash
esptool.py --chip esp32c3 --port COM3 flash_id
# покажет MAC, chip rev, и Detected flash size: 4MB
```

Параметр `esp32.flash_size` в YAML должен совпадать с этим значением. Несовпадение → ENOSPC при записи.

### Прочие инциденты разработки

Подробные технические разборы — `json:111` на DevKitC без PSRAM (v0.3.0-broken-no-log-false), lit-fix на v3 `sorting_groups` (v0.3.0-step6b), `Auth Expired` на esp-idf + ручной `scan_parameters: 600/180` (INC-12), сознательное исключение `log: true` на S3 — см. [README.md](README.md), раздел «Стабильность платформ — почему `arduino` на S3, почему `log: false` на классике».

---

## 4. Частые ошибки установки клиентов

См. [INSTALL.md](INSTALL.md), раздел **Troubleshooting** — там пошаговый разбор 10+ типовых проблем (драйвер CH340/CP210x, COM5 = SoundBlaster, `UnicodeDecodeError` с кириллицей в пути, BOOT-handshake, `Auth Expired`, OTA после смены пароля, `json:111`, `Invalid key format` для `api_encryption_key`).

**Самая частая (≈30 % обращений):** `Invalid key format, please check it's using base64.` на этапе `esphome compile`. Лечится одной командой:

```bash
python -c "import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())"
```

Полученную 44-символьную base64-строку положить в `secrets.yaml` как `api_encryption_key:`, заменив весь placeholder. Та же команда работает и для `ota_password` (или короче — `openssl rand -base64 32`).

---

## 5. Связанные скиллы / cross-references

- [atomfast-esp32 KNOWN_ISSUES](https://github.com/VibeEngineering-LLC/atomfast-esp32/blob/main/KNOWN_ISSUES.md) — аналогичный справочник для AtomFast (инцидент INC-C3-001 идентичен).
- [radex-esp32 README — Стабильность платформ](README.md) — полный технический разбор json:111, lit-fix sorting_groups, Auth Expired esp-idf coex.
- [Issues tracker](https://github.com/VibeEngineering-LLC/radex-esp32/issues) — сюда репортить новые проблемы или дополнения в матрицу плат.
