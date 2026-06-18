# Firmware changelog — radex MR107ion gateway

Версионирование: semver (`vMAJOR.MINOR.PATCH`).
- MAJOR — несовместимое изменение протокола / архитектуры
- MINOR — добавлена функциональность, обратная совместимость не нарушена
- PATCH — bugfix / стабильность / refactor без изменений API

Дата — UTC+3 (Europe/Moscow).


## v0.3.0-step8 (2026-06-15) — изоляция стабильности Radex: `bluetooth_proxy` закомментирован

**Что:** в основной `firmware/radex_gateway.yaml` блок `bluetooth_proxy: active: true`
закомментирован. На плате остаются только наш собственный `ble_client` к Radex
MR107ion и инфраструктура сервис-пояса (Web UI v3 + sorting_groups + auth +
log:false + Народмон-стек выключенный). `esp32_ble.max_connections: 4` оставлен
без изменений, чтобы вернуть `bluetooth_proxy` обратно одной строкой.

**Почему:** оператор: «нужно пока закрыть стандартный прокси? проверим стабильность
только на Радексе». Цель — изолировать BLE-стабильность нашего `ble_client`
(READ-poll 4 handle'ов каждые 4.3 с) от конкуренции `bluetooth_proxy` за air
time + Bluedroid task. Если на длительном прогоне реконнекты и heap-headroom
ведут себя лучше, чем при `active: true` — это управленческий аргумент в пользу
сценария «единый шлюз без btproxy» (вариант A в дорожной карте CLAUDE.md) для
радон-/радиационных приборов; параллельный bluetooth_proxy остаётся опциональным
дополнением.

**Изменения:**
- `firmware/radex_gateway.yaml`: блок `bluetooth_proxy:` оформлен комментарием
  с явным указанием «раскомментировать обратно при возврате к unified-gateway
  сценарию» и ссылкой на CLAUDE.md → «Дорожная карта».
- `esp32_ble.max_connections: 4` оставлен (одна оставшаяся слот-ниша = наш
  `ble_client`, ещё 3 свободны при возврате btproxy).
- Прочая конфигурация не тронута: web_server v3 + log:false + auth, sorting_groups
  Данные/Сервис, BLE READ-poll 4 handle'ов, Народмон select 4 опции, кнопки
  Safe Mode / Factory reset / Переподключиться / Сброс счётчиков BLE / Отправить
  на Народмон сейчас.

**Народмон HARD HOLD сохраняется:** switch «Выгружать на Народмон» — OFF по
умолчанию; ни одна ветка `send_narodmon` не дёргается. Сценарий «JSON POST» в
select остаётся на полке (не валидирован end-to-end), `interval: 300s` не
проверяется в этом релизе — до явного снятия бана оператором.

**Валидация перед перемещением в подвал** (плата на рабочем столе COM8,
.127, аптайм 628 с):
- BLE Radex подключён = **ON**.
- BLE: успешных коннектов = **1**, BLE: реконнектов всего = **0**.
- Температура = **26.5 °C**, Влажность = **44 %** (round-robin READ-poll
  по 4 handle'ам стабилен).
- Switch «Выгружать на Народмон» = **OFF** (HARD HOLD).
- API подключён = **OFF** (HA не подключён в этой сессии — норма, не регрессия).
- 0 `json:111`, 0 ERROR, 0 reboot после initial.

**Перемещение в подвал** (2026-06-15 ~12:27 MSK): плата отключена от USB и
перенесена; после включения по 5V запустился пассивный мониторинг
(`scripts/basement_snap.sh`, опрос SSE `/events` каждые 5 минут с записью
ключевых метрик в `firmware/radex/.logs/basement_step8/_meta.log`). Первый
снапшот после загрузки: BLE = ON, WiFi RSSI = **-59 dBm** (на 14 дБ лучше, чем
-73 dBm на рабочем столе — антенна свободнее в подвале), aптайм 154 с
(подтверждает чистую перезагрузку при переключении питания).

**Файлы в публичном скилле** (для воспроизведения):
- `firmware/radex_gateway.yaml` (724 строки, this release).
- `firmware/include/esp_diag.h`, `firmware/include/radex_read_hook.h` (без изменений).
- `firmware/secrets.example.yaml` (placeholder'ы, без изменений).
- Папка `firmware/www/` (CSS/JS, мёртвый код при `local: true`, оставлен для
  случая `local: false`).

**Возврат `bluetooth_proxy` обратно:** в `radex_gateway.yaml` снять `# ` перед
строками `bluetooth_proxy:` и `  active: true`; rebuild + USB-upload. Никаких
других изменений не требуется.


## v0.3.0-step6g-v2e (2026-06-14) — альтернативный YAML: web_server v2 + name-prefixed тематический порядок строк

**Что:** альтернативный файл прошивки `firmware/radex_gateway_v2.yaml` к основной
`radex_gateway.yaml` (v3 step6g). На той же кодовой базе step6g (esp-idf, BLE
READ-poll 4 handle'ов, 4-методный select Народмон GET/POST/HTTPS/JSON, auth,
log:true), но Web UI собран на **`web_server: version: 2`** — одна плоская
таблица вместо двух тематических групп v3 (sg_data / sg_service).

Тематический порядок строк достигается **префиксами в `name:` каждой сущности**:
`1.x` Прибор → `2.x` BLE + Сеть → `3.x` Народмон → `4.x` Система. v2 сортирует
строки **алфавитно по `name`**, цифры `0-9` (ASCII 48-57) идут раньше любой
кириллицы, поэтому префиксы насильно задают тематические блоки в одной плоской
таблице.

**Почему:** оператор хотел альтернативу к v3 multi-table layout — единая
плоская таблица под сценарий «глянул всё разом», без переключения групп. v3
step6g остаётся стабильной публичной версией; v2e — параллельный вариант для
оператора, у которого предпочтение единого вида.

**Полный набор префиксов (24 сущности):**

| Группа | Префиксы | Сущности |
|---|---|---|
| 1.x ПРИБОР | 1.1 – 1.4 | ОА радона (последняя/среднее прибор), Температура, Влажность |
| 2.x BLE + СЕТЬ | 2.1 – 2.B | BLE подключён, BLE коннекты, BLE реконнекты, Переподключиться, Сбросить счётчики BLE, WiFi сигнал, Подключено к WiFi, WiFi BSSID, IP адрес, ESP32 MAC, API подключён |
| 3.x НАРОДМОН | 3.1 – 3.6 | Switch «Выгружать», Select «Способ отправки» (GET/POST/HTTPS/JSON), 3 имени метрик (R/T/H), Кнопка «Отправить сейчас» |
| 4.x СИСТЕМА | 4.1 – 4.3 | Время работы, Перезагрузить (Safe Mode), Сброс к заводским |

**WiFi:** одна основная сеть через `wifi.ssid` + `wifi.password` (`!secret`).
Без `bssid` pin'а и без `fast_connect` (он полезен только при bssid pin) — ESP
выбирает точку с лучшим RSSI среди BSSID одной SSID. AP fallback
`radex-gw Fallback` — как второй уровень.

**Известные ограничения** (важно):
- При `web_server.local: true` ESPHome 2026.5.3 v2 раздаёт `/0.css` и `/0.js`
  bundle'ы из `css_include`/`js_include`, но **HTML на них не ссылается** —
  они становятся **мёртвым кодом** в браузере. Тематический порядок строк
  поэтому строится **только** на префиксах `name:`, не на JS-reorder
  (`www/reorder_v2.js`) и не на CSS-лимите лога (`www/log_limit.css`).
- Чтобы вернуть кастом-JS/CSS — установить `web_server.local: false`
  (HTML ссылается на `/0.js`/`/0.css` + CDN `https://oi.esphome.io/v2/www.js`,
  требует интернет у клиента). Trade-off offline vs custom-bundle описан в
  esp32-dev SKILL.md.
- Файлы `www/reorder_v2.js` + `www/log_limit.css` в скилле сохранены для случая,
  если оператор переключит `local: false`, или для сборок ESPHome, где
  `local: true` поведение исправят.

**Валидация:**
- compile + USB upload SUCCESS, hash verified.
- 30-сек SSE-snapshot подтвердил все 24 сущности с префиксами в `name:`;
  BLE CONNECTED #1; round-robin READ-poll 4 handle'ов работает
  (radon_last/temp/humidity/radon_avg_device).
- 0 `json:111` в snapshot'е, 0 ERROR, 0 reboot.
- RSSI -39 dBm. Радон публикуется (last 11.0, avg_device 78.4, temp 26.1°C, hum 46%).
- Деплоен оператором в подвал для длительного теста стабильности.

**Когда выбирать v2e vs v3 step6g:**
- **v3 step6g** (`radex_gateway.yaml`, default) — две таблицы «Данные» и «Сервис»,
  sorting_groups. Подходит когда нужно разделение «что показывает прибор» vs
  «как себя чувствует шлюз».
- **v2e** (`radex_gateway_v2.yaml`) — одна плоская таблица с тематическими
  префиксами `1.x/2.x/3.x/4.x`, проще читается «всё разом». Подходит когда
  оператор не хочет переключаться между группами.

Обе версии — на одной кодовой базе step6g (один и тот же BLE-парсер, одинаковый
Народмон-стек, одинаковая диагностика). Различия — **только** Web UI layout.

**Файлы (новые в этом релизе):**
- `firmware/radex_gateway_v2.yaml` — основной альтернативный YAML.
- `firmware/www/reorder_v2.js` — JS-reorder для случая `local: false` (177 строк,
  shadow-DOM walker для `esp-app` → `esp-entity-table`, 4-группный GROUPS array,
  inline-style лимит `esp-log` 240px). При `local: true` — мёртвый код.
- `firmware/www/log_limit.css` — CSS лимит textarea#log 240px для случая
  `local: false`. При `local: true` — мёртвый код.

**Миграция с v3 step6g:**
- `secrets.example.yaml` — всё через !secret: `wifi_ssid` + `wifi_password` +
  `api_encryption_key` + `ota_password` + `web_server_auth_user/pass` + `radex_mac`.
- `include/` — БЕЗ ИЗМЕНЕНИЙ (`esp_diag.h` + `radex_read_hook.h`).
- `firmware.factory.bin` — несовместима с v3 step6g (структура NVS web_server v2
  отличается); полная USB-прошивка обязательна при первой миграции.


## v0.3.0-step6g (2026-06-14) — select narodmon_method GET/POST/HTTPS/JSON + reset_ble button + uptime в Данные

**Что:** в select-dropdown «Способ отправки на Народмон» добавлена 4-я опция **JSON POST** (POST на `http://narodmon.ru/json`, `Content-Type: application/json`, тело `{"devices":[{"mac":"...","sensors":[{"id":"...","value":...},...]}]}`). Полный набор опций теперь: HTTP GET / HTTP POST / HTTPS POST / JSON POST. Web UI разнесён на 2 таблицы (`sorting_groups: [sg_data, sg_service]`): «Данные» (BLE подключён, радон последняя/среднее, температура, влажность, **Время работы**) и «Сервис» (WiFi/BLE-счётчики/IP-MAC-SSID/5 кнопок/select narodmon_method/Switch Народмон). Добавлена кнопка `ble_counters_reset` («Сбросить счётчики BLE»). Полю Uptime убран `entity_category: diagnostic` + перевод имени → «Время работы», перенесён в группу «Данные» с весом 50.

**Почему:** оператор: «JSON POST добавь» + «сделай двумя таблицами: данные и сервисные функции» + «добавь кнопку сброса числа реконектов и поля время работы». Все 4 запроса в одной версии.

**Изменения:**
- `select.template narodmon_method`: 4 options, `initial_option: "HTTP GET"`, `restore_value: true`, `optimistic: true`.
- В `script: send_narodmon` добавлена 4-я ветка JSON POST (snprintf-формирование JSON-body с NaN-guard для temp/hum, `capture_response: true` + `on_response: lambda` логирует `JSON POST HTTP %d, body: %s`).
- `web_server: sorting_groups: [sg_data (weight 10), sg_service (weight 20)]`.
- Per-entity `web_server: {sorting_group_id, sorting_weight}` на каждой сущности.
- Button `ble_counters_reset` (icon `mdi:counter`, weight 130 в Сервис) — обнуляет `ble_connect_count` + `ble_disconnect_count`.
- Boot banner: `step6g (web_server v3 + auth + log:false + 2 sorting_groups + reset_ble + narodmon_method select GET/POST/HTTPS/JSON) booted`.

**Состояние валидации:**
- step6f (предшественник, 2026-06-14): валидирован 380 с реальным Chrome MCP с F5-штормом 12+ навигаций → json:111=**0**, ERROR=0, WARN=0, DISCONNECT=0. Heap headroom отличный. Все 4 BLE-handle публикуются round-robin (4.3 с × 4 = 17.2 с цикл): radon_last, radon_avg_device, temp, hum.
- step6g (этот): config valid, compile SUCCESS (20.48 с с `PYTHONIOENCODING=utf-8 PYTHONUTF8=1` workaround для UnicodeDecodeError на Cyrillic-path), USB upload OK, Web UI shadow-DOM walk подтверждает «JSON POST» в select, BLE-чтение работает. **End-to-end JSON POST upload к narodmon.ru ещё не подтверждён в UART** (триггер ручной — нажать кнопку «Отправить на Народмон сейчас» с выбранной опцией JSON POST → grep `JSON POST body:` + `HTTP %d, body:`).

**Урок (lesson learned):**
На Windows с Cyrillic-путём проекта `esphome compile` падает с `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xd0 ... colorama ansitowin32.write`. Корень — PlatformIO/colorama декодирует stdout в cp1251 по дефолту. **Лечение**: всегда префиксовать `PYTHONIOENCODING=utf-8 PYTHONUTF8=1 esphome compile|upload …` (соответствует глобальному правилу «Windows stdout — UTF-8» в `~/.claude/CLAUDE.md`).

**Известные проблемы:**
- End-to-end JSON POST upload не подтверждён инструментально. План — `curl --user <user>:<pass> -X POST "http://<ip>/select/narodmon_method/set?option=JSON%20POST"` + `curl --user ... -X POST "http://<ip>/button/narodmon_send_now/press"` + UART grep на `JSON POST body:` и `HTTP `. Если сервер вернёт `{"error":"OK","errno":200}` — JSON POST подтверждён, иначе откатить опцию.
- Heap-OOM при устойчивом 7×F5 + api log-forwarding — edge-case (реальный пользователь делает 1-2 reload в сессии). WiFi-watchdog (180 с) делает clean recovery. Решение по троттлингу логов отложено.


## v0.3.0-step6b (2026-06-14) — Web UI redesign под AtomFast-стиль (sorting_groups + RU-имена) + lit fix (sg_re dropped)

**Что:** Web UI Radex'а получил **4 секции в стиле AtomFast**: «Radex — показания», «Настройки», «Народмон», «Диагностика». Все сущности переведены на русский. Добавлена кнопка «Переподключиться к Radex» (lambda `id(ble_radex).disconnect()`).

**Почему:** оператор показал скрин Web UI AtomFast и попросил «сделать подобный дизайн». До step6 Radex выводил все сущности одним плоским списком на английском (radon_last, temp_radex, …). После step6 — структурированная страница, читается как у AtomFast.

**Изменения:**
- `web_server: sorting_groups:` блок с 4 группами (sg_main / sg_settings / sg_narodmon / sg_diag) и `sorting_weight` 10/20/30/80.
- Все 4 главных сенсора (`radon_last`, `radon_avg_device`, `temp_radex`, `humidity_radex`) → name=ОА радона (последняя), ОА радона (среднее, прибор), Температура, Влажность; uom=`Bq/m³` для радона; mdi-иконки.
- Диагностика (RSSI, uptime, BLE-счётчики, IP/MAC/SSID/BSSID) → RU-имена в `sg_diag`.
- Народмон-блок (switch + 3 text-метрики + кнопка «Отправить сейчас») → `sg_narodmon` w=1/2..4/10.
- Кнопки «Перезагрузить (Safe Mode)», «Сброс к заводским», **новая** «Переподключиться к Radex» → `sg_settings` w=1/2/3.
- Баннер: `Radex Gateway v0.3.0-step6b (Web UI redesign: sorting_groups + RU names, sg_re dropped — empty-group lit fix) booted`.

**Сюрприз посередине — lit-rendering fix (step6 → step6b):**

Первая попытка (step6) объявила 5 групп (включая `sg_re` «RE / Отладка BLE» — задел на будущее: туда планируется raw notify hex / GATT-таблица). В step6 ни одной сущности `sorting_group_id: sg_re` не было.

При открытии Web UI в реальном Chrome:
- SSE-snapshot отдавал ВСЕ 5 групп и 22 сущности корректно (curl-проверка).
- Но в браузере `esp-entity-table` оставался **пуст** (rowCount=0), консоль кричала `[EXCEPTION] Error: invalid template strings array` в `lit-html` (стек `Uo → oa → new we → Ie._$AC`).

Причина: ESPHome web_server v3 `local.js` строит lit-template **на группу**, и если приходит `sorting_group` event для группы, в которую ни одна `state`-event не указывает, lit-renderer падает на пустом template-strings array → рушит весь `esp-entity-table.render()`.

**Фикс step6b — однострочный**: убрать `sg_re` из `sorting_groups:` до того момента, когда в неё добавим первую сущность. Группа вернётся в step7 (когда заведём raw notify hex / GATT-таблицу).

**Замеры (v0.3.0-step5b vs v0.3.0-step6b на той же плате `AA:BB:CC:DD:EE:FF` / 192.168.x.x):**

| Метрика | step5b | step6b |
|---|---|---|
| **Рендер всех групп в браузере** | flat list, ASCII names | 4 RU-секции, как у AtomFast ✓ |
| **`Error: invalid template strings array`** | n/a | устранён удалением пустой sg_re ✓ |
| `json:111` под calm reload (real-user) | 0 | 0 ✓ |
| `json:111` под 6×reload+parallel шторм за 9 с | n/a (тест меньше) | 58 транзиентных, последний в 11:15:25 |
| `json:111` за 2+ мин после шторма (recovery) | n/a | **0** (куча восстанавливается) ✓ |
| heap free (стационарно) | 48-58 КБ | 48-58 КБ ✓ |
| heap largest (стационарно) | 32 КБ | 32 КБ ✓ |
| heap min_ever под штормом | n/a | 664 B (транзиент, recovered) |
| BLE DISCONNECT за окно (~4.5 мин) | 0 | 0 ✓ |
| WiFi-watchdog reboot | 0 | 0 ✓ |
| `send result -2` | 0 | 0 ✓ |

**Что осталось открытым (Task #4 в проекте, edge case):**

6×reload за 9 с — это **искусственный шторм** инструмента валидации, не сценарий реального пользователя. Реальный пользователь делает 1-2 reload в сессии, не 6×F5. При реальной нагрузке `json:111 = 0`. Под штормом `min_ever=664 B` — куча близко к нулю, но не падает в WiFi-handshake-fail каскад. WiFi-watchdog (180 с порог) пока не активировался ни разу. Решение по api log-forwarding throttle/heap-headroom отложено до повторения проблемы в эксплуатации.

**Урок (зафиксирован в проекте):**

- ESPHome web_server v3 + `sorting_groups`: **никаких пустых групп**. Объявил группу — в YAML должна быть минимум одна сущность с этим `sorting_group_id`. Иначе lit-rendering падает целиком (`invalid template strings array`).
- Валидация Web UI обязательна с **реальным браузером** (Chrome MCP) — `curl /events` отдавал корректный SSE-snapshot, и проблема была невидима до открытия страницы в браузере.

**Файлы:**
- `firmware/radex_gateway.yaml` (текущий = step6b).
- `firmware/archive/v0.3.0-step5b/radex_gateway.yaml` (прошлая стабильная).
- `firmware/archive/v0.3.0-broken-no-log-false/radex_gateway.yaml` (broken; до step5b).


## v0.3.0-step5b (2026-06-14) — HOTFIX: восстановлен `web_server: log: false` (был в комментарии, но отсутствовал в YAML)

**Что:** добавлена физически отсутствовавшая строка `log: false` в блок `web_server:`. До этого фикса прошивка v0.3.0 СОБИРАЛАСЬ С `log: true` (дефолт ESPHome), несмотря на комментарий «log:false убирает Debug Log panel и live-log SSE». Process-bug: текст фикса присутствовал, сама опция — нет.

**Почему:** скрин оператора показал на v0.3.0 живую Debug Log панель в Web UI + залпы `[E][json:111]: JSON document overflow` **синхронно с** `[component:431] http_request cleared Error flag` (Народмон-аплоад через TLS сжимал heap free=49 КБ → 13 КБ, largest=43 КБ → 9.7 КБ за миллисекунды). Триггером был не браузер, а http_request narodmon → TLS-аллокации съедали contiguous-блок, и параллельный SSE log-stream из Debug Log панели натыкался на provision-fail JsonDocument'а. Корень тот же (Debug Log SSE), но триггер шире, чем «load/F5 браузера» — любой heap-pressure-event (Народмон-аплоад, WiFi-флап) поднимает json:111, пока Debug Log live-stream активен.

**Изменения:**
- `web_server:` блок: добавлена строка `log: false` (комментарий v0.3.0-step5b объясняет, почему «фикс был в комментарии, но не в опции»).
- Баннер: `Radex Gateway v0.3.0-step5b (web_server.log=false restored, was missing despite comment) booted`.
- Никаких других изменений (только однострочка).

**Замеры (v0.3.0 vs v0.3.0-step5b на одной плате `AA:BB:CC:DD:EE:FF` / 192.168.x.x):**

| Метрика | v0.3.0 (скрин оператора) | v0.3.0-step5b |
|---|---|---|
| Debug Log панель в Web UI | присутствует, активно стримит | устранена |
| json:111 при http_request (Народмон) | 8+ событий синхронно с upload | **0** |
| json:111 при 5×F5 storm | (исторически) залпы | **0** |
| Народмон HTTP 200 OK | да | да (2/2 аплоада, ручной + auto) |
| BLE DISCONNECT | 0 | 0 |
| crash reboot rst:0x | 0 | 0 |
| task_wdt | 0 | 0 |
| send result -2 | 0 | 0 |
| Buffer full | 0 | 0 |
| web_server took warnings | sporadic | 1 (норма) |
| heap min_ever, B | 840 | 9752 (бутовый transient, не блокирует) |

**Валидация:** compile SUCCESS 35.5 с (RAM/Flash без изменений vs v0.3.0), flash USB COM10 успешно. UART-окно ~4 мин с реальным Chrome + 5×F5 + ручной триггер Народмон-аплоада через REST + 1 авто-аплоад через интервал — 0 ошибок, 25 publishes `radon_avg_device` за окно, 1 BLE CONNECTED без переподключений. Лог: `firmware/radex/.logs/v030_step5b_FINAL.log` (269 строк).

**Известные проблемы:** депрекейшн warning ESPHome 2026.5.3 — `Deprecated URL format: /button/send_to_narodmon_now - use entity name '/button/Send to Narodmon now' instead. Object ID URLs will be removed in 2026.7.0`. Не критично, REST путь с object_id работает.

**Урок (зафиксирован для всех будущих фиксов):** **«комментарий ≠ фикс»**. Если в комментарии описано «эта опция делает X», проверять что опция действительно ВЫПИСАНА в YAML, а не только описана. Симптом-критерий валидации `log: false` — в Web UI **отсутствует** табе/панель Debug Log + в UART **отсутствуют** залпы json:111 при любых heap-pressure-событиях (Народмон, F5, WiFi-флап). Headless-валидация по UART недостаточна — нужен реальный браузер ИЛИ реальный http_request-триггер.


## v0.3.0 (2026-06-14) — ПЕРЕСБОРКА С НУЛЯ от bt-proxy baseline, СТАБИЛЬНАЯ

**Что:** полная переработка прошивки от официального ESPHome `bluetooth-proxies/esp32-generic.yaml` (non-factory). Пошаговая бисекция причины `[E][json:111]` overflow — добавление датчиков по одному с валидацией РЕАЛЬНЫМ браузером + F5-штормом на каждом шаге. **`json:111` устранён конструктивно одной строкой `web_server.log: false`** (гипотеза оператора Дмитрия 2026-06-14, подтверждена бисекцией).

**Почему:** v0.1.14 с `web_server v2` снижала overflow ×12.6, но не убирала корень. Точная диагностика на v0.1.15 (с heap-инструментовкой) показала: корень — НЕ размер snapshot'а, НЕ sorting_groups, НЕ фрагментация кучи. Корень — **Debug Log panel в Web UI v3** подписана на `/events` SSE; каждое лог-сообщение становится отдельным JSON-event'ом → web_server SSE-буфер не дренируется при высокой частоте лог-сообщений (heap-log 5с + READ-poll 4.3с + WiFi-флапы) → ArduinoJSON document overflow → `json:111` → near-OOM → каскад WPA2 handshake fail. Лечение — выключить `log:` (убирает Debug Log панель и live-log SSE-стрим; state-events продолжают идти штатно).

**Архитектура:**
- `framework: esp-idf` (тот же стек, что в bt-proxy baseline).
- `esp32_ble_tracker` + `bluetooth_proxy: active:true` — пассивная HA-проброска adv'ов параллельно нашему `ble_client`.
- `web_server: version: 3 / log: false` — **корень json:111 устранён**. Возвращены `sorting_groups` (4 группы: sg_main / sg_narodmon / sg_settings / sg_diag) с sparkline-графиками.
- `CONFIG_LWIP_MAX_SOCKETS: "16"` — фикс `ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET` для исходящих `http_request`.
- `CONFIG_ESP_TASK_WDT_TIMEOUT_S: "60"` — фикс `SW_CPU_RESET` во время блокирующего sync `http_request.get` (default 5с убивал loop task).
- BLEClientNode `radex_read_hook.h` — round-robin READ-poll по 4 GATT handle'ам (период каждого 17.2с).

**Сенсоры (5 product + диагностика):**
- `radon_last` (handle 0x0049, float32 LE Bq/m³).
- `radon_avg_device` (handle 0x0040, float32 LE Bq/m³).
- `temp_radex` (handle 0x0058, uint16 LE / 10 → °C).
- `humidity_radex` (handle 0x005E, uint8 → %).
- Народмон switch + кнопка «Send to Narodmon now» + script + auto-upload (300с).
- Диагностика: WiFi RSSI / signal / BSSID / IP / MAC / SSID, BLE connects/disconnects/connected, ESP uptime, heap-сенсоры (free / largest / min_ever).

**ВЫКИНУТО окончательно (решение оператора 2026-06-14):**
- `radon_min` (handle 0x0052), `radon_max` (handle 0x0055) — НЕ возвращать.
- Ring buffers (`g_buf_min/hour/day`) и агрегаты 1ч/24ч/7д/мес/год/lifetime — НЕ возвращать. История остаётся в HA Recorder / Народмон.

**Валидация (РЕАЛЬНЫЙ Chrome + F5-шторм через Chrome MCP, каждый шаг):**
- STEP 0 (чистый bt-proxy + web-стек, 0 product-сенсоров) — 5×F5: json:111=**0**.
- STEP 1 (+radon_last) — 7×F5 + `log:false`: json:111=**0** (с `log:true` было 119).
- STEP 2 (+climate temp+humidity) — 6×F5: json:111=**0**, heap free min 41 932 B.
- STEP 3 (+диагностика) — F5: json:111=**0**.
- STEP 4c (+Народмон, LWIP=16, task_wdt=60s) — 6×F5: json:111=**0**, heap free min 34 972 B.
- STEP 5 (+radon_avg device 0x0040) — 6×F5 + 5+ мин прогона: **json:111=0, rst:0x=0, task_wdt=0, DISCONNECT=0, ERROR=0, send result=0, stuck EventSource=0, Buffer full=0**. radon_avg_device publish=8 (round-robin 17.2с). heap min: free=34 296 / largest=17 408 / min_ever=9 100 B.

**Замеры (RAM/Flash):**
- v0.3.0 / step5: RAM **22.2%** (72 776 B / 327 680), Flash **80.8%** (1 483 111 B / 1 835 008).
- vs v0.1.14: RAM ~совпадает, Flash +2.2% (esp-idf vs arduino, bt-proxy stack overhead).

**Известные проблемы (не блокируют, отдельные тики):**
- Edge-case: устойчивый 7×F5 шторм + параллельный `api log-forwarding` к HA может транзиентно просадить largest до ~1.1 КБ ниже WPA2 supplicant требования (~3-4 КБ) → handshake-fail-loop. WiFi-watchdog (180с) делает clean recovery через `arch_restart()`. Реальный пользователь делает 1-2 reload в сессии, НЕ 7×F5. Решение по троттлингу api log-forwarding'а (`logger: level: WARN` / снять heap-log) — отложено.
- Внешний narodmon.ru сервер периодически возвращает HTTP 503 / connection abort (тестировано независимо с PC: timeout 30s, exit 28). ESP-side стек полностью функционален; внешний сервер блокирует только end-to-end валидацию выгрузки.

**Migration с v0.1.x:**
- `secrets.example.yaml` — без изменений (5 полей: wifi_ssid/password, api_encryption_key, ota_password, radex_mac).
- include/: добавлены `esp_diag.h` (esp_heap_caps.h wrapper) и `radex_read_hook.h` (BLEClientNode READ-poll hook). Положить в `firmware/include/` рядом с YAML.
- При первом upload — полная USB-прошивка (несовместимая структура NVS из-за смены framework arduino → esp-idf).


## v0.1.14 (2026-06-13) — web_server v2 (Path B), радикальное сжатие json:111 overflow

**Что:** `web_server: version: 2` вместо v3. Убраны `sorting_groups` (5 групп: sg_main / sg_avg / sg_narodmon / sg_settings / sg_diag) и все per-entity блоки `web_server: { sorting_group_id, sorting_weight }`. Главная страница Web UI — плоская алфавитная таблица entity'ев без sparkline-графиков. BLE / NVS / Народмон / HA REST API / ESPHome API / агрегаты — без изменений.

**Почему:** v0.1.13 принята как стабильная (Path C — overflow в Debug Log признан безвредным), но оператор поймал реальный burst (50+ `[E][json:111]` за 17 сек на скриншоте Web UI) при одновременной активности нескольких клиентов (мой REST snap + оператор в браузере). Решено убить overflow в корне — отказаться от v3-broadcast-пайплайна с sorting_groups, который и был главным источником JSON-разрастания.

**Изменения:**
- `web_server: version: 3 → 2`, удалён массив `sorting_groups:` (5 групп).
- 30+ entity: убраны блоки `web_server: { sorting_group_id, sorting_weight }` через PowerShell `[regex]::Replace` (1022 байта YAML).
- Header / boot ESP_LOGI / useragent → `v0.1.14` / `1.14`.

**Замеры (USB COM8, 5.5-мин UART):**
- json:111 за 5-мин: **101 (v0.1.13) → 8 (v0.1.14)** — снижение ×12.6.
- При curl-snap /events 10 сек (полный snapshot 15 visible entities, 6249 байт): **0 overflow**.
- 1 чистый boot (POWERON 17:58:26), safe_mode counter reset через 1 мин (Boot seems successful).
- 0 reboot за 5.5 мин, BLE работает, радон публикуется (354.3 Bq/m³ last / 80.96 avg / temp 26.2 °C / hum 48 %).

**Диагноз остаточных 8 overflow:**
- НЕ размер entities (каждый /events JSON ≈ 200-300 байт, буфер ESPHome ≈ 1 KB).
- НЕ длинные кириллические имена (snapshot снят полностью без overflow).
- Race condition в `broadcastState()` web_server v2 при **multiclient activity** (одновременный SSE + REST polling). ArduinoJSON-буфер shared между broadcast-нитями; второй клиент не дожидается освобождения буфера → overflow одного пакета. Падает только пакет SSE-event'a, HA перезапросит через REST — функциональность не нарушена.

**Решение (принято оператором 2026-06-13):** v0.1.14 принята **как стабильная** для подвального 24h-прогона. В HA-режиме клиент один → race condition минимален → overflow ожидается ≈ 0. Если в production-эксплуатации (HA-only, без Web UI) overflow повторится — следующий шаг v0.1.15 = D-вариант (отключение Web UI / ещё внутренние entities). 

**Trade-off Path B vs Path A:**
- Path B (v0.1.14, выбран): теряем sorting_groups и sparkline-графики на главной Web UI странице. Все agg/диагностика по-прежнему есть как entity'и.
- Path A (отложен): остаёмся на web_server v3 + ждём ESPHome PR на увеличение JSON-буфера.

**Известные проблемы:**
- ~1.6 overflow/мин при одновременной активности 2+ клиентов (мой curl + HA REST + Web UI). НЕ блокирует BLE/Народмон/HA — только теряются отдельные SSE-event'ы (HA получит state на следующем REST poll).
- Web UI без sparkline-графиков — все trends смотреть в HA dashboard или родном приложении Radex.

**Прошивка:**
- USB COM8 (через CH340 1A86:7523 суффикс `&0&4`), 1.44 MB compressed → 965 KB, 22.4 сек, 514 kbit/s, hash verified.
- OTA попытка через WiFi/3232 — провалилась (web_server v3 overflow в v0.1.13 блокировал OTA thread). USB-fallback — рекомендованный путь для миграции с overflow-проблем v3.

---
## v0.1.13 (2026-06-13) — досжатие JSON overflow (year/lifetime/buf_*_fill → internal)

**Что:** ещё 5 entity выведены в `internal: true` — `radon_avg_year`, `radon_avg_lifetime`, `buf_min_fill`, `buf_hour_fill`, `buf_day_fill`. Visible-набор Web UI сократился с ~17 до ~12 (sg_main 6 + sg_avg 4 + sg_narodmon 4 + sg_settings 2). Все агрегаты и диагностика по-прежнему доступны через ESPHome API / HA.

**Почему:** v0.1.12 убрал 2 счётчика (Народмон sent/fail) — недостаточно. UART прогон поймал **34× `[E][json:111]`** в пачке `16:11:22-23` в момент подключения нового клиента (HA через `aioesphomeapi`). Причина — full state-sync дамп всех visible entities в один ArduinoJSON-буфер ≈1 KB; кириллические имена («Уровень опасности», «Тренд (1 ч)») особенно тяжёлые (UTF-8 ×2). Web UI Debug Log показывал только 1 запись (фильтрация дублей на стороне frontend) — UART не врёт.

**Изменения:**
- `radon_avg_year` + `radon_avg_lifetime` → `internal: true`, удалены `web_server` блоки.
- `buf_min_fill` + `buf_hour_fill` + `buf_day_fill` → `internal: true`, удалены `web_server` блоки.
- Header / boot ESP_LOGI / useragent → `v0.1.13` / `1.13`.

**Доступ к скрытым значениям:**
- `radon_avg_year` / `lifetime` — через ESPHome API / HA sensor / REST `/sensor/radon_avg_year` / `/sensor/radon_avg_lifetime`.
- `buf_*_fill` — через REST `/sensor/buf_min_fill` и т.п. (для отладки заполнения буферов).

**Замеры:**
- json:111 за 5-мин прогон: **34 (v0.1.12) → 101 (v0.1.13)** — гипотеза «много entities» опровергнута. Причина шире: при full state-sync `web_server: v3 + sorting_groups` ArduinoJSON-буфер не вмещает дамп даже при 12 visible entities. Воспроизводится в момент подключения нового клиента (HA через `aioesphomeapi`, refresh браузера).
- Прошивка стабильна: 0 reboot, radon публикуется каждые 30s (70.33→74.33 Bq/m³), BLE/WiFi работают, агрегаты идут.
- Visible Web UI entity: 17 → ~12.
- Логика Народмон + агрегаты + NVS — без изменений.

**Решение (принято оператором 2026-06-13):** v0.1.13 принята **как стабильная (Path C)** — json:111 это шум в Debug Log при reconnect клиента, **функциональность не нарушена**. Альтернативы (откат на `web_server: v2` без sorting_groups или ждать ESPHome PR на JSON buffer size) — отложены.

**Известные проблемы:**
- `[E][json:111] JSON document overflow` пачкой 30-100 за секунду при подключении нового web/HA клиента к ESPHome API + web_server v3. Не влияет на работу — `radon_*`, агрегаты, switch Народмон, REST API, ESPHome API publishing — всё работает. Web UI рендерится, HA получает все entities.
- Если позже понадобится чистый лог — откатиться на `web_server: version: 2` (как в v0.1.2/v0.1.5).

---

## v0.1.12 (2026-06-13) — фикс JSON document overflow в Web UI

**Что:** Народмон-счётчики (`narodmon_sent`, `narodmon_fail`) помечены `internal: true` — убраны из Web UI. Значения остаются доступны через ESPHome API / Home Assistant. Количество видимых entity в web_server v3 уменьшено с ~22 до ~20 → ArduinoJSON-буфер инициального state-sync больше не переполняется.

**Почему:** на v0.1.11 в Web UI Debug Log массово ловили `[E][json:111] JSON document overflow` (8× за один boot/refresh). Default-буфер ArduinoJSON в web_server v3 ≈1 KB; при ~22 видимых entities + длинных названиях («Radon avg 24h», «Уровень опасности», «Buf hour fill») инициальный snapshot не помещается. Аналогично фиксу `ble-explorer v0.1.1` (#53) — удалением длинных entity из publish-set.

**Изменения:**
- `narodmon_sent` + `narodmon_fail`: `internal: true`, удалены блоки `web_server.sorting_group_id/sorting_weight`.
- Header / boot ESP_LOGI / useragent → `v0.1.12` / `1.12`.

**Замеры:**
- json:111 за 5-мин прогон: **8 (v0.1.11) → 0 (v0.1.12, цель)** — валидируется отдельно.
- Видимые entity в Web UI: 22 → 20.
- Логика Народмон (HTTP POST, OK/fail инкременты) — без изменений; счётчики продолжают расти и видны через API.

**Известные проблемы:**
- Если оператор захочет видеть OK/fail в Web UI — поднять json-document-size через external_component, либо вынести в отдельную страницу диагностики.

---

## v0.1.11 (2026-06-13) — расширенные ring buffers + window-агрегаты
**Что:** ёмкости ring buffers увеличены в десятки раз с **запасом 14-50% над целевым окном** — старое вытесняется FIFO автоматически по мере заполнения (round-robin). Все агрегаты переведены на **окно последних N точек**, не на всю длину буфера — поэтому расширение capacity увеличивает горизонт хранения без искажения смысла "24h"/"7d"/"30d"/"365d". Добавлены 3 diagnostic sensor'а заполнения буферов (`Buf min/hour/day fill`) в sg_diag.

**Почему:** оператор: «для объёмной активности радона Radon last данные усреднять за указанные периоды. И хранить их максимально долго. По мере заполнения вычищать старые данные оставляя запас». Старые ёмкости (60/24/365) точно совпадали с длинами окон → буфер вечно "под завязку", запаса для предыстории нет. Новые ёмкости (90/10000/4400) обеспечивают **до 12 лет day-средних в RAM** + 1.5 час raw минут + ≈14 месяцев hour-средних.

**Бюджет RAM ring buffers:**
| Buffer | Cap (точек) | Granularity | Горизонт | RAM |
|---|---|---|---|---|
| g_buf_min | 90 | 1 мин | 1.5 час | 360 B |
| g_buf_hour | 10000 | 1 час | ≈14 мес | 40 KB |
| g_buf_day | 4400 | 1 день | ≈12 лет | 17.6 KB |
| **Итого** | | | | **≈58 KB** |

Heap ESP32 ≈250 KB → ≈190 KB остаётся свободным. Запас комфортный.

**Изменения:**
- `substitutions:` добавлены `buf_min_cap=90 / buf_hour_cap=10000 / buf_day_cap=4400`.
- `globals` буферов — те же `std::vector<float>`, `restore_value: false`. Cap-checks в lambda заменены литералов на `${buf_*_cap}`.
- on_time `*/1` minute → push в `g_buf_min` (cap 90).
- on_time `0` minute (часовой) → push hour-avg в `g_buf_hour` (cap 10000), **24h avg = mean(last 24 of hour_buf)**, lifetime Welford — без изменений.
- on_time `0` minute `0` hour (суточный) → **day_avg = mean(last 24 of hour_buf)** (раньше — весь буфер), push в `g_buf_day` (cap 4400). **7d/30d/365d = mean(last N of day_buf)**.
- Новые sensor'ы `buf_min_fill / buf_hour_fill / buf_day_fill` в sg_diag (sorting_weight 20-22) — показывают `vec.size()`. Видно сколько точек уже накоплено.

**Persistence (что переживает reboot):**
- ✅ Финализированные средние (`g_nvs_avg_hour/24h/7d/month/year`) — float NVS scalar.
- ✅ Lifetime running mean (`g_nvs_lifetime_mean` + `g_nvs_lifetime_count`) — продолжает с предыдущего состояния через Welford.
- ✅ Народмон switch state + счётчики OK/FAIL.
- ❌ **Raw history в ring buffers RAM — теряется при reboot**. После reboot средние видны (из NVS), но горизонт начинает накапливаться заново. Полная NVS-blob персистенция буферов — отдельная задача (можно сделать через ESP-IDF `nvs_set_blob` если оператор подтвердит).

**Замеры:**
- `esphome config` → ✅ `INFO Configuration is valid!`
- compile + flash + UART валидация — pending

**Известные проблемы:**
- При первой записи 4400-элементного буфера в NVS-blob (если будем делать persistence) — ~18 KB одной транзакцией. NVS поддерживает, но wear-cycle растёт. Альтернатива — periodic snapshot раз в N часов, не на каждый push.

---

## v0.1.10 (2026-06-13) — Народмон switch + 6 NVS-агрегатов радона (включая lifetime)
**Что:** возвращён Народмон auto-upload как опциональный switch (по умолчанию OFF, интервал **1 час**) + добавлены **6 средних значений** ОА радона: за час / 24 часа / 7 дней / 30 дней / 365 дней / **всё время работы прибора** (lifetime running mean — Welford, NVS-persisted). Архитектура без cascade-фильтров (v0.1.9 урок task_wdt сохранён): `time:sntp + on_time triggers` + `globals: std::vector<float>` ring buffers в RAM + 7 NVS-persisted scalar для восстановления после reboot.

**Почему:** оператор (1): «добавь переключатель выгрузки в Народмон, среднее значение ОА за час, 24 часа, 7 дней, месяц, год. Проверь как используется память ESP32». Оператор (2, уточнение): «добавь в прошивку — хранить в памяти среднее за сутки, неделю, месяц, год, за всё время. Выгружать раз в час». Анализ памяти (из firmware.elf v0.1.9): DRAM .data+.bss = 71.6 KB / 320 KB → ~250 KB heap, Flash app = 1.20 / 1.75 MB → 550 KB запас, NVS partition = 384 KB запас. Бюджет агрегатов (~2 KB RAM + ~2 KB NVS) — пренебрежимо.

**Изменения:**
- `logger.logs.web_server: ERROR` — подавлены deprecated URL warnings и ANSI-мусор из httpd на вкладке Debug Log.
- `time:sntp.on_time:` 3 cron-trigger'а — `*/1 minute` (минутный сэмпл radon_last → buf), `0 minute` (финализация hour + 24h), `0 hour 0 minute` (финализация day + 7d + 30d + 365d).
- `globals`: 3 ring buffers (`g_buf_min` cap 60, `g_buf_hour` cap 24, `g_buf_day` cap 365) типа `std::vector<float>` RAM-only + 5 NVS scalar (`g_nvs_avg_hour/24h/7d/month/year`, sentinel `-1.0f`) с `restore_value: true` + **2 NVS scalar для lifetime running mean** (`g_nvs_lifetime_mean: float`, sentinel `-1.0f`; `g_nvs_lifetime_count: uint32_t`).
- 6 новых `sensor: platform: template` (`radon_avg_hour` / `radon_avg_24h` / `radon_avg_7d` / `radon_avg_month` / `radon_avg_year` / **`radon_avg_lifetime`** mdi:infinity) в `sg_avg` группе.
- **Lifetime running mean** (Welford-style): обновляется на каждой часовой финализации через `lm += (avg - lm) / count_new`. Сэмпл — финализированный часовой avg (≤1 NVS write/час → wear-friendly). Безопасный счётчик до ~190 лет аптайма (uint32_t).
- Группы `sg_avg` + `sg_narodmon` вернулись в `web_server.sorting_groups`.
- `http_request:` блок возвращён (с `request_headers:` вместо deprecated `headers:`).
- `switch: template "Народмон upload"` (optimistic, restore_mode RESTORE_DEFAULT_OFF).
- `button: "Send to Народмон"` — ручной триггер `script.execute send_narodmon`.
- `script: send_narodmon` — формирует body `#MAC\n#RAD1#value#Radon last#Bq/m3\n#T1#value#Temp#C\n#H1#value#Humidity#%\n##` и POST на `http://narodmon.ru/get`, статус → счётчики `g_narodmon_sent` / `g_narodmon_fail` в NVS.
- 2 новых sensor (Narodmon sent OK / Narodmon fail) в `sg_narodmon`.
- `interval: ${narodmon_interval_s}s` (**1 час, narodmon_interval_s="3600"**) gated на `switch.is_on: narodmon_enabled`. Раньше планировался 5 мин — изменено по требованию оператора (снижает нагрузку на narodmon.ru и trafic).
- `interval: 10s` дополнен восстановлением 6 агрегатов (включая lifetime) и 2 счётчиков из NVS если sensor ещё не публиковался (post-boot recovery).

**Анализ памяти (приложен в чате):**
| Регион | Used | Total | Запас |
|---|---|---|---|
| DRAM .data + .bss | 71.6 KB | 320 KB | ~250 KB heap |
| IRAM .text | 103 KB | 128 KB | OK |
| Flash app | 1.20 MB | 1.75 MB | 550 KB |
| NVS | <1 KB | 384 KB | громадный |

**Замеры (pending):**
- `esphome config` → ✅ `INFO Configuration is valid!`
- compile + flash COM8 + 5-мин UART валидация
- Народмон switch ON, POST на narodmon.ru → счётчик OK инкрементируется

**Известные проблемы:**
- Народмон требует привязки MAC ESP32 в личном кабинете narodmon.ru (без token — анонимный device id). Если не привязан — server вернёт 200 OK но данные не сохранит.
- Агрегаты hour/24h работают только после NTP sync (5-30 сек post-boot).
- Buffer теряется при reboot, finalized scalars восстанавливаются из NVS.

---

## v0.1.9 (2026-06-13) — убраны sliding_window cascade-фильтры (гипотеза task_wdt)
**Что:** удалены `radon_hour_avg` (`platform: copy` + `throttle_average: 60s` + `sliding_window_moving_average window:60`) и `radon_day_avg` (`throttle_average: 3600s` + `window:24`). Удалены группы `sg_avg` + `sg_narodmon` из `web_server.sorting_groups`. Видимых entities стало 7 (radon×4, temp, hum, ble_connected) — совпадает с v0.1.2 baseline.

**Почему:** в v0.1.3-v0.1.8 устойчиво воспроизводился reboot-loop 60-120 сек (см. `DEBUG_LOG.md`). Опровергнуты: http_request (v0.1.7), active scan (v0.1.8), wifi.power_save_mode, api.reboot_timeout. Самая сильная оставшаяся гипотеза (DEBUG_LOG #1): task_wdt (5 сек на arduino) — sliding_window cascade на каждом publish дёргает длинную лямбду → main loop > 5 сек → reset. v0.1.2 был стабилен (3.5 disconnect/6мин с переподключением, без reboot) именно без этих фильтров.

**Изменения:**
- removed: `radon_hour_avg` (`platform: copy` → throttle_average 60s → sliding_window window 60)
- removed: `radon_day_avg` (`platform: copy` → throttle_average 3600s → sliding_window window 24)
- removed: `sg_avg`, `sg_narodmon` из `web_server.sorting_groups`
- comments-only: explanatory header `# v0.1.9: hour_avg + day_avg удалены (sliding_window cascade — главный подозреваемый по task_wdt 5 сек)`
- target visible entities = 7 (radon last/avg/min/max + temp + hum + ble_connected)
- все диагностические entity сохраняют `internal: true` из v0.1.4+

**Замеры:**
- `esphome config radex_gateway.yaml` → `INFO Configuration is valid!` (2026-06-13)
- compile, flash, 10-мин validation pending (ICMP + REST poll 1 Hz на 192.168.x.x)

**Гипотеза-проверка:**
- ✅ Если reboot-loop исчез → DEBUG_LOG hypothesis #1 (task_wdt из-за sliding_window cascade) подтверждена.
- ❌ Если flap остался → ищем дальше (brownout, web_server v3 + encryption, плата/железо).

**Известные проблемы (v0.1.9):**
- Нет hour_avg / day_avg агрегатов в Web UI. Если стабильность подтвердится — вернуть, но без `platform: copy` cascade, а как Home Assistant statistics или короткий `throttle_average` без sliding_window.
- Народмон-выгрузка ещё не реализована (планировалась в v0.1.3, выпилена в v0.1.7 при дебаге). Добавить после стабилизации.

---

## v0.1.8 (2026-06-13) — passive scan-параметры (гипотеза active scan jammed WiFi)
**Что:** в `esp32_ble_tracker.scan_parameters` выставлено `active: false`.
**Почему:** v0.1.7 без http_request — тот же flap → отбрасываем http_request. Следующая гипотеза: высокий duty active scan забивает антенну 2.4 GHz, WiFi-старвейшн.
**Замеры:** flap не исчез. Скриншот «started 43 seconds ago» → **reboot-loop**, не network glitch. USB serial не успел снять причину reset (logger через WiFi не доступен в reboot-окне).
**Опровергнуто:** active scan как первопричина flap'а.

---

## v0.1.7 (2026-06-13) — удалён весь Народмон-блок (гипотеза http_request виноват)
**Что:** убраны `http_request`, `script.upload_narodmon`, `button.upload_narodmon_now`, `switch.narodmon_enabled`, `interval` 300 с.
**Почему:** в v0.1.6 при подключении к Web UI первая минута давала 3 JSON document overflow + 3 disconnect. Гипотеза: http_request конкурирует за heap с web_server v3.
**Замеры:** та же картина: ALIVE на +30 с (11:42:32), DEAD на +90 с (11:44:10), `WinError 121`. ICMP тоже пропадает → не TCP/JSON, а WiFi-уровневое выпадение.
**Опровергнуто:** http_request не виноват.

---

## v0.1.6 (2026-06-13) — откат js_include + замер 10-мин flapping
**Что:** убран `js_include: log_popup.js`. Возвращён `scan 1100/350 ms`. Логгер DEBUG → файл, 10 мин прогон.
**Почему:** проверить v0.1.5-инжекцию виновата в пустом экране, и собрать аккуратный профиль flap.
**Замеры:** Web UI alive на +30 с, dead на +90 с (HTTP timeout 10 с). 10-мин логгер: **3 disconnect, 3 JSON document overflow burst** в первую минуту. Flapping воспроизводится строго.
**Доказано:** инжекция JS виновата только в чёрном экране, не в flap'е. Flap — отдельный класс.

---

## v0.1.5 (2026-06-13) — добавлен log_popup.js (debug-инструмент)
**Что:** `web_server.js_include: log_popup.js` — popup-окно «Debug Log» в Web UI через MutationObserver на `subtree:true`.
**Почему:** нужно было видеть события disconnect/JSON-overflow в самом Web UI, не отдельным `esphome logs`.
**Замеры:** **пустой экран** Web UI. MutationObserver на `subtree:true` поломал Vue SPA.
**Процессный урок:** для injection в готовый Vue/Svelte UI — только CSS-инжекция, без mutation observer на subtree.

---

## v0.1.4 (2026-06-13) — удалены week/month/year агрегаты + text: блоки
**Что:** убраны три самых широких `sliding_window`: `week_avg`/`month_avg`/`year_avg` (последний с `window: 365`). Удалены лишние `text:` блоки. Все 10 диагностических entities (BLE counters, MAC, SSID, RSSI, uptime'ы, IP) получили `internal: true` (Path B из v0.1.1 «Следующий шаг»). Видимых entities ≈ 9.
**Почему:** в v0.1.3 при boot — `[E][json:111] JSON document overflow` flood + `safe_mode` reboot-loop. Главный подозреваемый — `year_avg` с window 365 (огромный sliding-buffer + энкодинг JSON-кэша на старте web_server v3).
**Замеры:** boot-loop устранён, `safe_mode` → «Boot seems successful». Web UI открывается. **Но**: `unexpected disconnect API` каждые ~90 с. «Зелёная лампа справа вверху не горит».
**Доказано:** year_avg/month_avg/week_avg — слишком тяжёлые для ESP32 (RAM). Удалены окончательно.
**Открыто:** flapping API/Web UI каждые 90 с.

---

## v0.1.3 (2026-06-13) — расширение функциональности (+5 sliding_window, http_request, Народмон)
**Что:** добавлены:
- 5 cascade-фильтров sliding_window: `hour_avg`, `day_avg`, `week_avg`, `month_avg`, `year_avg` (window 60/24/7/30/365)
- `http_request` для выгрузки на narodmon.ru
- `script.upload_narodmon` + `button.upload_narodmon_now`
- `switch.narodmon_enabled`
- `interval: 300s` для авто-выгрузки
- `web_server.version: 3` + `sorting_groups`
**Почему:** v0.1.2 был стабилен, но базовый. Нужны агрегаты для трендов + Народмон-выгрузка.
**Замеры:** **boot-loop**. При boot — flood `JSON document overflow`, main loop hang, `safe_mode` reboot каждые ~30 с.
**Опровергнуто:** ESP32 + arduino не тянет 14 entities + sliding_window 365 в web_server v3 одновременно.

---

## v0.1.2 (2026-06-13) — базовый рабочий сетап (baseline для бисекта)
**Что:** возврат к минимуму: `scan_parameters: 1100/350 ms passive`, без агрегатов, без http_request, без Народмона, `web_server.version: 2`, ASCII-имена.
**Почему:** v0.1.1 показала, что ASCII-rename не лечит overflow → нужен «голый» baseline, чтобы дальше делать бинарный поиск виновника.
**Замеры:**
- **Стабилен**: 6-мин прогон, 2 disconnect (с автореконнектом за ~7 с), 0 reboot.
- Web UI отзывчив, API alive, BLE качает sensor'ы.
- Используется как **reference baseline** для всех v0.1.3+ гипотез.
**Доказано:** ESP32-DevKitC + arduino + ESPHome 2026.5.3 + Radex MR107ion BLE — рабочая комбинация при минимальной нагрузке.

---

## v0.1.1 (2026-06-13) — fix JSON document overflow в Web UI
**Что:** PATCH-фикс `[E][json:111]: JSON document overflow` → битая Web UI.
Те же симптомы что в ble-explorer v0.1.0 (см. ble-explorer/CHANGELOG.md v0.1.1).

**Почему:** в v0.1.0 кириллические имена sensor'ов в Web UI v3 (например
`"BLE disconnects (реконнектов всего)"` — 40+ байт UTF-8), кириллические
`unit_of_measurement` (`"Бк/м³"`, `"сек"`) и `sorting_groups.name`
(`"Radex — радон + климат"`) переполняли ArduinoJson-буфер SSE-стрима
web_server при подключённом UI-клиенте. Симптом:
```
[E][json:111]: JSON document overflow (массово, при открытом /events)
```

**Изменения:** все user-visible строки в Web UI переведены на ASCII:
- sensor.name: `"Radon last (последнее)"` → `"Radon last"` (и аналогично avg/min/max/temp/hum/uptime/BLE).
- unit_of_measurement: `"Бк/м³"` → `"Bq/m3"`, `"сек"` → `"s"`.
- sorting_groups.name: `Radex — радон + климат / Настройки / Диагностика` → `Main / Settings / Diag`.
- button.name + text_sensor.name (WiFi/IP/SSID/restart/reconnect/reset) — ASCII.

**Замеры (USB-flash COM8, 2026-06-13 02:46, тот же MAC AA:BB:CC:DD:EE:FF):**
- `[E][json:111]: JSON document overflow` — **продолжается** (~непрерывный поток
  при подключении к Web UI).
- `[W][web_server_idf:681]: Closing stuck EventSource connection after 2500 failed sends` — SSE-стрим переполнен.
- ESPHome API: `unexpected disconnect ... WinError 121` — main loop под нагрузкой.
- HTTP GET `http://192.168.x.x/` → `HTTP ERROR 503 Service Unavailable`
  (белый экран в браузере).
- BLE+sensors при этом работают штатно:
  `radon=101.67 Bq/m3`, min=49.8, max=77.0, temp=26.6, hum=49, BLE connects=7,
  disconnects=1, session растёт; reboot=0.

**ROOT CAUSE — НЕ длина имён, а количество entities × web_server v3 SSE encoder.**
ASCII-rename был ложным следом. Гипотеза была от ble-explorer v0.1.1 (там лечилось
`internal: true` на длинных text_sensor `scan_list`/`g_gatt_dump`/`notify_view`).
В radex длинных text_sensor нет вообще, проблема не в длине строк, а в том, что
web_server v3 рендерит каждый sensor publish в ArduinoJson-документ с обёрткой
`{id, state, value, sorting_group, ...}`; при ~22 entities (radon×4, BLE counters×3,
RSSI×2, uptime×2, IP, MAC, SSID, temp, hum, ble_connected, 3 button, restart, switch,
WiFi RSSI) суммарный SSE event на каждый publish превышает буфер v3 SSE encoder.

**Известные проблемы (v0.1.1):**
- Web UI 503 — НЕ исправлен. Прошивка функционально работает (BLE+sensors+API),
  но Web UI непригоден.
- v0.1.1 НЕ выгружается в публичный скилл radex-ble (правило CLAUDE.md:
  ≥5 мин стабильного прогона без критичных проблем).

**Следующий шаг — v0.1.2 (планируется в новой сессии):**
1. **Path A**: `web_server.version: 2` — даунгрейд на v2 SSE (без `sorting_groups`).
   Atomfast v0.6.0 на v2 работает без overflow при сопоставимом числе entities.
   Минус: UI попроще, плоский список.
2. **Path B**: `internal: true` на 12-15 диагностических entities
   (BLE counters, MAC, SSID, RSSI, uptime'ы, IP). В Web UI остаются 7 главных
   (radon×4, temp, hum, ble_connected). Те что internal — доступны через
   ESPHome API → Home Assistant и REST.
3. **Path C**: `web_server:` отключить полностью, оставить `api:` + `captive_portal:`.

Рекомендация для следующей сессии — **B** (живой Web UI с главными метриками
для дебага + полный набор полей через HA), либо **A** как страховка.

---

## v0.1.0 (2026-06-13) — первый успешный прогон на железе
**Боевая верификация:** прошита 2026-06-13 02:17 на ESP32-DevKitC через
COM8, подключение к реальному MR107ion `AA:BB:CC:DD:EE:FF`. Логи:
- `[02:17:39][I][radex:409]: radon=30.33 Bq/m3` — live OAR_last
- min/max совпали с btsnoop (49.8 / 77.0)
- BLE connects=2, disconnects=0, session растёт
- 0 reboot, все 7 sensor'ов опубликовали значения

**Известная проблема v0.1.0** → закрыта в v0.1.1: `JSON document overflow`
в web_server из-за длинных UTF-8 имён.

**Что:** первая boilerplate-прошивка для Radex MR107ion.
**Почему:** GATT-профиль декодирован (см. radex-ble/references/mr107ion.md),
нужна рабочая ESPHome-прошивка для шлюза в Web UI / Home Assistant / Народмон.
**Изменения:**
- BLE-клиент с MAC из !secret radex_mac, auto_connect: true
- READ-poll цикла 7 ключевых handles сервиса FE651700 каждые ~4.3 с
  (полный цикл ~30 с, как RadexM-приложение)
- 7 sensor'ов: Radon last/avg/min/max (Бк/м³), Temp, Humidity, Uptime
- BLEClientNode hook (include/radex_read_hook.h) для перехвата READ_CHAR_EVT
- BLE-стабильность: scan 32% duty + счётчики (как atomfast v0.6.0)
- RSSI sensor через on_ble_advertise
- Web UI с группами sg_main/sg_settings/sg_diag
**Замеры:**
- (отсутствуют, прошивка ещё не прошита на устройство)
**Известные проблемы:**
- handle-based read привязан к ревизии 0214 MR107ion. На других ревизиях
  handles могут отличаться — тогда переключиться на UUID-based read
  (FE6517NN-...) через ble_client characteristic platform.
- Не реализована выгрузка на Народмон (добавить аналогично atomfast v0.6.0).
- Не валидирован на реальном устройстве. До первого успешного 5-мин
  прогона — НЕ выгружать в публичный скилл radex-ble (правило CLAUDE.md).