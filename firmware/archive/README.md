# Архив прошивок radex-esp32

Снимки прошлых версий, на случай отката. Каждая подпапка — копия
`firmware/radex_gateway.yaml` ровно перед заменой на следующую версию.

Семантика версий и подробное описание изменений — в основном
[`../CHANGELOG.md`](../CHANGELOG.md). Здесь — только короткие пометки
«что и почему» для быстрой навигации.

> ⚠️ **Все архивные YAML содержат `restore_mode: RESTORE_DEFAULT_ON`** на switch
> `narodmon_enabled` — это **до** фиксации HARD-правила проекта (2026-06-17:
> switch Народмона ОБЯЗАН быть `ALWAYS_OFF` во всех новых прошивках). При
> использовании архивного YAML — **обязательно** замени `RESTORE_DEFAULT_ON`
> на `ALWAYS_OFF`, иначе после ребута / safe-mode / factory_reset / OTA switch
> вернётся во **включённое** состояние, и плата начнёт отправлять данные на
> Народмон. Образец актуального паттерна — `../radex_gateway_s3.yaml`.

| Версия | Папка | Дата | Когда применять / почему отказались |
|---|---|---|---|
| v0.3.0-broken-no-log-false | [v0.3.0-broken-no-log-false/](v0.3.0-broken-no-log-false/) | 2026-06-14 | Первая v0.3.0 после rebuild от bt-proxy baseline. `web_server.log: false` упомянут в комментарии, но **в YAML фактически отсутствовал** → DevKitC без PSRAM при открытом браузере + http_request на Народмон уходил в каскады `json:111: JSON document overflow`, Web UI грузился пустым. Не использовать. Снимок оставлен для разбора INC. |
| v0.3.0-step5b | [v0.3.0-step5b/](v0.3.0-step5b/) | 2026-06-14 | HOTFIX: `web_server.log: false` физически выписан в YAML. `json:111` устранён, Web UI снова рендерится. Альтернативный «безопасный» baseline, если на новых сборках стало жрать heap. |
| v0.3.0-step6b | [v0.3.0-step6b/](v0.3.0-step6b/) | 2026-06-14 | Web UI redesign: 4 sorting_groups (BLE / Data / Service / Reverse Engineering) с русскими именами + lit fix (пустая `sg_re` выкинута, чтобы lit-element не падал на пустой группе). База для последующих UX-инкрементов. |
| v0.3.0-step6e | [v0.3.0-step6e/](v0.3.0-step6e/) | 2026-06-14 | Откат `web_server: version: 3` → `version: 2` + HTTP Basic Auth (`auth.username`/`auth.password` из secrets) + `wifi.networks` (multi-SSID fallback). База для всех последующих v0.3.0-step6X с auth + multi-SSID. |

Версии до v0.3.0 (этапы bt-proxy baseline-experiments) — смотри
в git-истории по commit-хешам:

- **v0.1.x (включая v0.1.14)**: самые ранние snapshots — изначальный
  bluetooth-proxy fork от ESPHome official `bluetooth-proxy/esp32-generic.yaml`
  до выделения `radex-esp32` в собственный скилл и собственный кодовый поток.
  Жил в общем `ESP32/firmware/_baseline_btproxy/` и доступен только через
  `git log -- _baseline_btproxy/` / `git show <commit>:<path>`.
- **v0.2.x bt-proxy baseline**: до rebuild от 2026-06-14 — этапы освоения
  BLE-стека и единого `ble_client` под Radex.
- **v0.3.0 rebuild от bt-proxy baseline**: первая выкладка, далее `step5b/6b/6e`
  (выше) и `step6f/step6g/step8` в основном файле.

Чтобы достать любой YAML из git:

```bash
git show <commit>:radex-esp32/firmware/radex_gateway.yaml > old.yaml
```

## Когда обновлять архив

При каждой выкладке новой стабильной прошивки в `firmware/radex_gateway.yaml`:

1. Создать `archive/<old-version>/radex_gateway.yaml` (копия старого файла).
2. Добавить строку в таблицу выше — версия, папка, дата, одна фраза почему отказались.
3. Тогда уже коммитить и пушить новую версию.

**Архив только для классической прошивки** (`radex_gateway.yaml` на DevKitC + esp-idf).
S3-baseline (`radex_gateway_s3.yaml` step3 и далее, arduino + PSRAM)
пока не архивируется — это вторая параллельная ветка, снимки старых
версий хранятся в git-истории.
