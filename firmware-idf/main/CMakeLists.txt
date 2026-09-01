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
