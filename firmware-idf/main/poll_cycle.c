#include "poll_cycle.h"
#include "radon_stats.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "time.h"

static const char *TAG = "poll_cycle";

// Допустимые значения цикла в минутах
static const uint32_t VALID_CYCLES[] = {10, 20, 30, 60};
static const size_t NUM_VALID_CYCLES = sizeof(VALID_CYCLES) / sizeof(VALID_CYCLES[0]);

// Статический массив для хранения последних 12 промежутков
static int32_t intervals[12];
static size_t interval_count = 0;
static size_t interval_index = 0;

// Последнее время обновления
static bool  have_prev = false;
static float prev_radon = 0.0f;
static time_t last_update = 0;

// Ручная установка цикла
static uint32_t manual_cycle_minutes = 0;

// Флаг для отслеживания стабильности оценки
static bool is_stable = false;

// Вспомогательная функция для поиска ближайшего допустимого значения
static uint32_t find_closest_cycle(uint32_t value)
{
    uint32_t closest = VALID_CYCLES[0];
    int32_t min_diff = abs((int32_t)value - (int32_t)VALID_CYCLES[0]);
    
    for (size_t i = 1; i < NUM_VALID_CYCLES; i++) {
        int32_t diff = abs((int32_t)value - (int32_t)VALID_CYCLES[i]);
        if (diff < min_diff) {
            min_diff = diff;
            closest = VALID_CYCLES[i];
        }
    }
    
    return closest;
}

// Вспомогательная функция для вычисления медианы
static uint32_t calculate_median(int32_t *arr, size_t count)
{
    // Создаем копию массива для сортировки
    int32_t sorted[12];
    memcpy(sorted, arr, count * sizeof(int32_t));
    
    // Сортируем копию (простая сортировка пузырьком)
    for (size_t i = 0; i < count - 1; i++) {
        for (size_t j = 0; j < count - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                int32_t temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }
    
    // Вычисляем медиану
    if (count % 2 == 1) {
        return sorted[count / 2];
    } else {
        return (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
    }
}

// Вспомогательная функция для проверки стабильности
static bool is_cycle_stable(void)
{
    if (interval_count < 3) {
        return false;
    }
    
    // Берем последние три значения и проверяем, что они округляются к одному значению
    uint32_t last_three[3];
    for (int i = 0; i < 3; i++) {
        size_t idx = (interval_index + interval_count - 3 + i) % interval_count;
        last_three[i] = find_closest_cycle(abs(intervals[idx]));
    }
    
    return (last_three[0] == last_three[1]) && (last_three[1] == last_three[2]);
}

void poll_cycle_init(void)
{
    /* Последнее значение берём из файла: состояние отсева живёт в ОЗУ,
       и после перезагрузки первое пришедшее значение считалось новым —
       в истории появлялись дубли (найдено на живых данных 29.08). */
    float last = radon_stats_last_radon();
    if (!isnan(last)) { prev_radon = last; have_prev = true; }

    // Читаем сохранённый цикл из NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("radon", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint32_t saved_cycle = 0;
        err = nvs_get_u32(nvs, "cycle", &saved_cycle);
        if (err == ESP_OK && saved_cycle != 0) {
            manual_cycle_minutes = saved_cycle;
            ESP_LOGI(TAG, "Загружен ручной цикл: %u минут", (unsigned)manual_cycle_minutes);
        }
        nvs_close(nvs);
    }
    
    // Сброс статистики
    interval_count = 0;
    interval_index = 0;
    last_update = 0;
    is_stable = false;
}

bool poll_cycle_observe(time_t ts, float radon)
{
    /* Значение считается НОВЫМ только если оно отличается от прошлого. Прибор
       пересчитывает радон циклами 10/20/30/60 минут, а мы опрашиваем чаще —
       без этой проверки одно измерение попало бы в историю несколько раз и
       перекосило бы среднее (оно стало бы взвешено по частоте опроса, а не по
       времени). Сравнение по разнице, а не на равенство: значения приходят
       float32, точное сравнение на них ненадёжно. */
    if (!have_prev) {
        have_prev = true;
        prev_radon = radon;
        last_update = ts;
        return true;                 /* первое значение записываем, промежутка ещё нет */
    }

    if (fabsf(radon - prev_radon) <= 0.005f) {
        return false;                /* прибор ещё не пересчитал — это повтор */
    }

    int32_t interval = (int32_t)(ts - last_update);
    prev_radon = radon;

    /* Промежуток кладём в кольцо из 12 последних. Отрицательный или нулевой
       (часы подвинулись синхронизацией) не считаем — он исказил бы медиану. */
    if (interval > 0) {
        intervals[interval_index] = interval;
        interval_index = (interval_index + 1) % 12;
        if (interval_count < 12) interval_count++;
    }

    last_update = ts;
    is_stable = is_cycle_stable();
    return true;
}

uint32_t poll_cycle_minutes(void)
{
    if (manual_cycle_minutes != 0) {
        return manual_cycle_minutes;
    }
    
    if (interval_count < 3) {
        return 0; // Не достаточно данных для оценки
    }
    
    uint32_t median = calculate_median(intervals, interval_count);
    return find_closest_cycle(median);
}

uint32_t poll_cycle_next_delay_ms(void)
{
    uint32_t cycle_minutes = poll_cycle_minutes();
    
    if (cycle_minutes == 0) {
        // Если цикл ещё не определён, используем среднее значение (30 минут)
        return 10 * 60 * 1000; // 10 минут
    }
    
    // Вычисляем момент следующего обновления: последнее обновление + цикл + 60 секунд запаса
    time_t next_update_time = last_update + cycle_minutes * 60 + 60;
    
    // Вычисляем задержку в миллисекундах
    time_t now = time(NULL);
    int32_t delay_seconds = next_update_time - now;
    
    // Если время уже прошло, опрашиваем сразу (но не менее 60 секунд)
    if (delay_seconds <= 0) {
        return 60 * 1000; // 60 секунд
    }
    
    // Ограничиваем задержку от 60 секунд до 65 минут
    uint32_t delay_ms = delay_seconds * 1000;
    if (delay_ms < 60 * 1000) {
        return 60 * 1000;
    }
    if (delay_ms > 65 * 60 * 1000) {
        return 65 * 60 * 1000;
    }
    
    return delay_ms;
}

void poll_cycle_set_manual(uint32_t minutes)
{
    // Проверяем корректность значения
    if (minutes != 0 && minutes != 10 && minutes != 20 && minutes != 30 && minutes != 60) {
        ESP_LOGW(TAG, "Некорректное значение цикла: %u минут. Игнорируем.", (unsigned)minutes);
        return;
    }
    
    manual_cycle_minutes = minutes;
    
    // Сохраняем в NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("radon", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "cycle", minutes);
        if (err == ESP_OK) {
            nvs_commit(nvs);
            ESP_LOGI(TAG, "Сохранён ручной цикл: %u минут", (unsigned)minutes);
        } else {
            ESP_LOGE(TAG, "Ошибка сохранения в NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs);
    } else {
        ESP_LOGE(TAG, "Ошибка открытия NVS: %s", esp_err_to_name(err));
    }
    
    // Если устанавливаем 0, то сбрасываем ручную установку и начинаем обучение
    if (minutes == 0) {
        ESP_LOGI(TAG, "Возвращаемся к автоматическому определению цикла");
        interval_count = 0;
        interval_index = 0;
        last_update = 0;
        is_stable = false;
    }
}

int poll_cycle_json(char *buf, size_t len)
{
    uint32_t cycle_minutes = poll_cycle_minutes();
    time_t now = time(NULL);
    
    // Определяем стабильность
    bool stable = is_cycle_stable();
    
    int written = snprintf(buf, len,
        "{\"minutes\":%u,\"manual\":%s,\"samples\":%u,\"last_update\":%lld,\"stable\":%s}",
        (unsigned)cycle_minutes,
        manual_cycle_minutes != 0 ? "true" : "false",
        (unsigned)interval_count,
        (long long)last_update,
        stable ? "true" : "false"
    );
    
    return written;
}
