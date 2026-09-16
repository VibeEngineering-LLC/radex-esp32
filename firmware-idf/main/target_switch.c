/* #RADEX-283: оператор 16.09 сменил прибор вручную последовательностью «test finish ->
   tests/save -> test start»; теперь это делает прошивка сама при POST /api/target с MAC,
   отличным от текущего. Тот же MAC — ничего. Первый выбор прибора — замеры не трогаются.
   Метка после санитайзера платы — только [A-Za-z0-9_-], экранирование в JSON не нужно. */
#include "target_switch.h"
#include "target_label.h"
#include "ble_radex.h"
#include "radon_stats.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <esp_log.h>

static const char *TAG = "target";

int radex_target_switch(const char *mac_body, char *out, size_t out_sz, bool *restart) {
    if (restart) *restart = false;
    if (out == NULL || out_sz == 0) return 400;

    char norm[13];
    if (mac_body == NULL || !radex_mac_normalize(mac_body, norm)) {
        snprintf(out, out_sz, "{\"ok\":false,\"error\":\"bad mac\"}");
        return 400;
    }

    char old_mac[18] = {0};
    bool had = ble_radex_target_mac(old_mac, sizeof(old_mac));

    if (had && radex_mac_same(old_mac, mac_body)) {
        ESP_LOGI(TAG, "прибор тот же — замер не трогаю");
        snprintf(out, out_sz, "{\"ok\":true,\"changed\":false}");
        return 200;
    }

    if (!ble_radex_set_target(mac_body)) {
        snprintf(out, out_sz, "{\"ok\":false,\"error\":\"bad mac\"}");
        return 400;
    }

    if (restart) *restart = true;

    if (!had) {
        ESP_LOGI(TAG, "первый выбор прибора — замеры не трогаю");
        snprintf(out, out_sz, "{\"ok\":true,\"restart_ms\":1500,\"changed\":true,\"finished\":false,\"saved\":null,\"new_test_start\":0}");
        return 200;
    }

    bool finished = radon_stats_finish_test();
    time_t saved_start = 0;
    uint32_t points = 0;
    char label[RADEX_TL_LABEL_MAX + 1] = {0};

    if (finished) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char want[RADEX_TL_LABEL_MAX + 1];
        if (!radex_target_switch_label(old_mac, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, want, sizeof(want)))
            snprintf(want, sizeof(want), "pribor-smena");
        saved_start = radon_stats_test_snapshot(&points);
        if (saved_start > 0) {
            if (!radon_stats_test_label_set(saved_start, want))
                ESP_LOGE(TAG, "имя замера не записано");
            radon_stats_test_label_get(saved_start, label, sizeof(label));   /* что реально легло в реестр */
        } else {
            ESP_LOGE(TAG, "замер завершён, но снимок не сохранён");
        }
    }

    bool started = radon_stats_start_test();
    time_t new_start = started ? radon_stats_test_start() : 0;

    ESP_LOGW(TAG, "смена прибора: завершён=%s, сохранён start=%lld (%lu изм., \"%s\"), новый замер=%lld%s",
             finished ? "да" : "нет", (long long)saved_start, (unsigned long)points, label, (long long)new_start,
             started ? "" : " (часы не синхронизированы — не начат)");

    if (saved_start > 0) {
        snprintf(out, out_sz, "{\"ok\":true,\"restart_ms\":1500,\"changed\":true,\"finished\":%s,\"saved\":{\"start\":%lld,\"points\":%lu,\"label\":\"%s\"},\"new_test_start\":%lld}",
                 finished ? "true" : "false", (long long)saved_start, (unsigned long)points, label, (long long)new_start);
    } else {
        snprintf(out, out_sz, "{\"ok\":true,\"restart_ms\":1500,\"changed\":true,\"finished\":%s,\"saved\":null,\"new_test_start\":%lld}",
                 finished ? "true" : "false", (long long)new_start);
    }

    return 200;
}
