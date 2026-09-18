# REQUIRES намеренно НЕ задаётся. Компонент main по умолчанию зависит от всех
# компонентов сборки; явный список (как в CMakeLists донора) обрезал зависимости
# и ломал линковку Bluedroid — undefined reference to esp_ble_gattc_open.
# У донора BLE нет, поэтому его список зависимостей нам не годится.
# #USB-1: канал связи с прибором — Kconfig RADEX_TRANSPORT (main/Kconfig.projbuild).
# USB-транспорт реализует ТЕ ЖЕ функции ble_radex.h / ble_radex_journal.h, поэтому
# собирается ВМЕСТО BLE-файлов; всё выше шва (веб, API, история, журнал) не меняется.
if(CONFIG_RADEX_TRANSPORT_USB)
    set(RADEX_TRANSPORT_SRCS "radex_usb.c" "radex_ekosf.c")
else()
    set(RADEX_TRANSPORT_SRCS "ble_radex.c" "ble_radex_journal.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        ${RADEX_TRANSPORT_SRCS}   # #USB-1: BLE или USB — см. выбор выше
        "radex_data.c"
        "web_server.c"
        "ha_mqtt.c"
        "narodmon.c"
        "radon_stats.c"
        "poll_cycle.c"
        "log_ring.c"
        "wifi_manager.c"
        "improv_serial.c"
        "net_time.c"
        "http_io_gate.c"
        "radex_journal_parse.c"   # #RADEX-281: чистый разбор, хост-тест test/host
        "radex_journal_track.c"   # #RADEX-281: привязка квитанций/notify к шагу, хост-тест
        "target_label.c"          # #RADEX-283: имя замера и сравнение MAC, хост-тест
        "target_switch.c"         # #RADEX-283: смена прибора с сохранением замера
        "label_utf8.c"            # #RADEX-274: имя замера с кириллицей, хост-тест
        # radon_test_guard.h — static inline, отдельного .c нет (#RADEX-291)
    INCLUDE_DIRS "."
    EMBED_FILES
        "../web/index.html.gz"    # #RADEX-294: сжатая страница + web/index_etag.h от build_page.py
        "../web/setup.html"
)

# ── Локальная сборка «для себя» (W-063) ───────────────────────────────────
# Определение RADEX_GW_LOCAL_WIFI включает в main.c подключение
# main/secrets_local.h, то есть кладёт в образ реальные SSID и пароль.
# Задаётся ТОЛЬКО кэш-переменной CMake из build.sh (`idf.py -DRADEX_GW_LOCAL_WIFI=1`);
# значение всегда передаётся явно (0 или 1), поэтому старое значение в кэше
# не переживает следующую сборку — «забыл сбросить» здесь невозможно.
# Публиковать такой образ нельзя: он помечен маркером в .rodata и отвергается
# гейтом scripts/check_binary_secrets.py.
if(RADEX_GW_LOCAL_WIFI)
    message(WARNING
        "RADEX_GW_LOCAL_WIFI=1: в образ попадут сетевые данные из "
        "main/secrets_local.h. Такой образ НЕ ПУБЛИКОВАТЬ.")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RADEX_GW_LOCAL_WIFI=1)
endif()
