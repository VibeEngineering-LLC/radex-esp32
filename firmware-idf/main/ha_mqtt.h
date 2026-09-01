// Интеграция с Home Assistant через MQTT Discovery (#GW-1).
//
// Сущности HA заводит сам по retained-конфигам в топиках
// homeassistant/sensor/<id>/config — руками в HA настраивать ничего не нужно.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Поднимает MQTT-клиент, если в NVS задан адрес брокера. Без него — тихо не
// делает ничего: MQTT опционален, шлюз работает и без Home Assistant.
void ha_mqtt_start(void);

// Публикует текущие показания. Вызывается по таймеру; без соединения молчит.
void ha_mqtt_publish(void);

bool ha_mqtt_enabled(void);
bool ha_mqtt_connected(void);

// Настройки брокера: mqtt://host:1883 (пустой uri выключает интеграцию).
bool ha_mqtt_set_config(const char *uri, const char *user, const char *pass);
int  ha_mqtt_config_json(char *buf, size_t len);
