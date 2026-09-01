# REQUIRES намеренно НЕ задаётся. Компонент main по умолчанию зависит от всех
# компонентов сборки; явный список (как в CMakeLists донора) обрезал зависимости
# и ломал линковку Bluedroid — undefined reference to esp_ble_gattc_open.
# У донора BLE нет, поэтому его список зависимостей нам не годится.
idf_component_register(
    SRCS
        "main.c"
        "ble_radex.c"
        "radex_data.c"
        "web_server.c"
        "ha_mqtt.c"
        "narodmon.c"
        "radon_stats.c"
        "poll_cycle.c"
        "log_ring.c"
        "wifi_manager.c"
        "net_time.c"
        "http_io_gate.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "../web/index.html"
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
