# radex-esp32 — Общие паттерны линейки Quarta-Rad / Radex (полное тело)

Извлечено из `SKILL.md` при lean-рефакторе #CTX-4 (2026-07-11), провенанс verbatim.
Наблюдения по mr107ion + другим приборам линейки. Краткая суть (READ-poll, база FE651Y00)
продублирована в SKILL.md; здесь полные тела.

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
