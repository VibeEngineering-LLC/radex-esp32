// #RADEX-294: причина прошлой перезагрузки и uptime до неё (NVS "boot").
#pragma once
#include <stdint.h>

void boot_info_init(void);                 // после nvs_flash_init
void boot_info_tick(void);                 // периодически; в NVS не чаще раза в 10 мин
int boot_info_last_reset_reason(void);     // esp_reset_reason_t этого старта
uint32_t boot_info_prev_uptime_s(void);    // 0 — прошлый старт не прожил 10 мин
