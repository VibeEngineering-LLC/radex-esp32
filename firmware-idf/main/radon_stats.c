/*
 * Источник: «Оценка соответствия помещений зданий требованиям Норм радиационной безопасности
 * по ограничению содержания радона в воздухе. Методика радиационного контроля (проект)»,
 * АНРИ, 2025, №1(120), с. 76–95; практическое руководство — Цапалов А.А. и др., там же.
 *
 * Эталонная реализация: скилл radon-rational-method (rational_method.py), контур «Радоновый риск».
 * Перенос на 2026-08-28
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>   /* sqrt в критерии (1) методики */
#include <sys/stat.h>
#include <esp_spiffs.h>
#include <esp_timer.h>   /* #RADEX-113: относительные метки до синхронизации */
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>   /* ESP_ERR_NVS_NOT_FOUND */
#include <dirent.h>      /* #RADEX-102: обход /data за файлами замеров */
#include "radon_stats.h"

void radon_stats_bump_generation(void);   /* #RADEX-172, тело ниже */
static const char *TAG = "radon_stats";
static bool mounted = false;
#ifdef RADEX172_STRESS_WRITER
/* #SA-3, мутационный стенд. Только для ручных сборок: модуль переводится на
   ОТДЕЛЬНЫЙ файл, чтобы нагрузочный прогон со второй пишущей задачей не трогал
   метрологическую историю оператора (71 точка замера радона по методике
   Цапалова — портить её ради проверки нельзя). Топология гонки при этом та же:
   одна задача пишет, задача httpd читает тот же файл. */
static const char *filename = "/data/stress.csv";
#else
static const char *filename = "/data/radon.csv";
#endif

// Таблицы из методики Цапалова
static const struct {
    int days;
    float uv;
    float kp;
} normal_ventilation[] = {
    {4, 1.25f, 1.74f}, {5, 1.20f, 1.72f}, {6, 1.20f, 1.70f}, {7, 1.20f, 1.69f},
    {8, 1.20f, 1.68f}, {10, 1.10f, 1.67f}, {12, 1.10f, 1.66f}, {14, 1.10f, 1.65f},
    {20, 1.10f, 1.61f}, {30, 1.05f, 1.56f}, {60, 1.00f, 1.48f}, {90, 0.85f, 1.44f},
    {120, 0.65f, 1.42f}, {150, 0.55f, 1.37f}, {180, 0.45f, 1.31f}, {210, 0.35f, 1.24f},
    {240, 0.25f, 1.20f}, {270, 0.17f, 1.14f}, {300, 0.10f, 1.09f}, {330, 0.05f, 1.05f},
    {360, 0.00f, 1.00f}
};

static const struct {
    int days;
    float uv;
} restricted_ventilation[] = {
    /* 2 суток = 1.05 по Приложению А источника. Сверено с таблицей построчно
       28.08: в первой генерации здесь стояло 0.95 — заниженная неопределённость
       на самом коротком тесте, то есть завышенная уверенность в заключении. */
    {2, 1.05f}, {3, 1.00f}, {4, 0.95f}, {5, 0.90f}, {6, 0.80f}, {7, 0.75f}, {8, 0.70f},
    {10, 0.65f}, {12, 0.60f}, {14, 0.55f}, {20, 0.50f}, {30, 0.45f}, {60, 0.40f},
    {90, 0.38f}, {120, 0.36f}, {150, 0.32f}, {180, 0.26f}, {210, 0.20f}, {240, 0.16f},
    {270, 0.14f}, {300, 0.09f}, {330, 0.05f}, {360, 0.00f}
};

// Проверка на корректность времени
static bool is_valid_time(time_t ts) {
    return ts >= 1700000000; // после 2023 года
}

// Инициализация файловой системы и создание заголовка файла
bool radon_stats_init(void) {
    /* #RADEX-172: мьютекс создаём ПЕРВЫМ делом — до монтирования и независимо
       от его исхода. Ленивое создание «при первом обращении» само было бы
       гонкой: две задачи вошли бы в него одновременно и получили два разных
       мьютекса. Вызов radon_stats_init() стоит в app_main ДО web_server_init()
       и ble_radex_start(), то есть до появления вторых читателей/писателей. */
    if (!radon_stats_mutex_create()) {
        ESP_LOGE(TAG, "#RADEX-172: не создан мьютекс истории — работа без защиты от гонки");
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Не удалось смонтировать SPIFFS: %s", esp_err_to_name(ret));
        return false;
    }

    mounted = true;

#ifdef RADEX172_STRESS_CLEANUP
    /* ВРЕМЕННО: разовая уборка файлов мутационного стенда #SA-3. */
    remove("/data/stress.csv");
    remove("/data/stress2.csv");
    ESP_LOGW(TAG, "#SA-3: файлы стенда удалены");
#endif

    // Проверяем существование файла
    FILE *f = fopen(filename, "r");
    if (!f) {
        // Создаем файл с заголовком
        f = fopen(filename, "w");
        if (f) {
            fprintf(f, "time,radon,radon_avg,temp,hum\n");
            fclose(f);
        } else {
            ESP_LOGW(TAG, "Не удалось создать файл %s", filename);
        }
    } else {
        fclose(f);
    }

    return true;
}

// Добавление записи в файл
/* #RADEX-172: прототипы внутренних (под мьютексом) реализаций.
   Сгенерированы scripts/radex172_rename_locked.py, руками не править. */
static void radon_stats_add_locked(time_t ts, float radon, float radon_avg, float temp, float hum);
static int radon_stats_rebase_locked(time_t now);
static bool radon_stats_period_locked(uint32_t seconds_back, radon_period_t *out);
static void radon_stats_assess_range_locked(time_t from, time_t to,
                              float c_rl, float u_d, bool restricted,
                              radon_assess_t *out);
static int radon_stats_json_locked(char *buf, size_t len, float c_rl, float u_d, bool restricted);
static bool radon_stats_start_test_locked(void);
static bool radon_stats_reset_locked(void);
static time_t radon_stats_test_start_locked(void);
static time_t radon_stats_test_start_eff_locked(void);
static int radon_stats_tests_list_json_locked(char *buf, size_t len);
static int radon_stats_test_points_json_locked(time_t start, char *buf, size_t len);
static float radon_stats_last_radon_locked(void);
static int radon_stats_compact_locked(uint32_t raw_days, uint32_t hour_days);

static void radon_stats_add_locked(time_t ts, float radon, float radon_avg, float temp, float hum) {
    if (!mounted) return;
    /* #RADEX-113: пока часы не синхронизированы, запись НЕ отбрасывается, а
       получает относительную метку — отрицательные секунды от включения платы.
       Прежнее поведение теряло данные безвозвратно: плата без интернета мерила
       и выбрасывала всё до тех пор, пока кто-нибудь не установит время, а
       прибор календарного времени не отдаёт вовсе (проверено по карте
       характеристик MR107ion).
       Отрицательные значения выбраны намеренно: они не пересекаются с epoch и
       ни одна старая запись не может быть принята за относительную. Когда время
       появится, radon_stats_rebase() пересчитает их в реальные один раз. */
    if (!is_valid_time(ts)) {
        int64_t up = (int64_t)(esp_timer_get_time() / 1000000);
        ts = (time_t)(-up);
        if (up == 0) return;   /* самая первая секунда — отличить не от чего */
    }

    // Проверяем размер файла
    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size > 1800000) { // 1.8 МБ
        ESP_LOGI(TAG, "Файл переполнен, прореживаем...");
        FILE *f_in = fopen(filename, "r");
        if (!f_in) return;

        char temp_filename[] = "/data/radon_temp.csv";
        FILE *f_out = fopen(temp_filename, "w");
        if (!f_out) {
            fclose(f_in);
            return;
        }

        // Пишем заголовок
        char line[128];
        if (fgets(line, sizeof(line), f_in)) {
            fputs(line, f_out);
        }

        int count = 0;
        while (fgets(line, sizeof(line), f_in)) {
            if (count % 2 == 0) {
                fputs(line, f_out);
            }
            count++;
        }

        fclose(f_in);
        fclose(f_out);

        // Заменяем оригинальный файл
        remove(filename);
        rename(temp_filename, filename);
        radon_stats_bump_generation();   /* #RADEX-172: файл пересобран */

        ESP_LOGI(TAG, "Прорежено, осталось %d строк", count / 2 + 1);
    }

    FILE *f = fopen(filename, "a");
    if (f) {
        fprintf(f, "%lld,%.2f,%.2f,%.1f,%.0f\n", (long long)ts, radon, radon_avg, temp, hum);
        fclose(f);
    }

    /* #RADEX-102: дублируем запись в файл активного замера, если он есть.
       ts проверяем на положительность отдельно от общего is_valid_time() выше:
       замер начинается только при синхронизированных часах (start_test это
       гарантирует), но эта функция может получить относительную метку от
       НЕзависимого источника (например повторный вызов после сбоя) - пишем
       в файл замера только реальные метки, иначе имя файла (по tstart) и
       содержимое разъедутся по эпохам. */
    time_t ts_active = radon_stats_test_start();
    if (ts_active > 0 && ts >= ts_active) {
        char tf[48];
        radon_stats_test_file(ts_active, tf, sizeof(tf));
        if (tf[0]) {
            FILE *tf_f = fopen(tf, "a");
            if (tf_f) {
                fprintf(tf_f, "%lld,%.2f,%.2f,%.1f,%.0f\n",
                        (long long)ts, radon, radon_avg, temp, hum);
                fclose(tf_f);
            }
        }
    }
}

/* #RADEX-113: перевод относительных меток в реальные. Вызывается ОДИН раз, когда
   время наконец стало известно: каждая запись с отрицательной меткой t получает
   real = now - (uptime_now - |t|), то есть восстанавливается по разнице моментов
   включения. Точность ограничена точностью самих часов платы — это заведомо
   лучше, чем прежняя потеря данных целиком.

   Файл переписывается через временный: правка на месте оборвалась бы при потере
   питания посреди прохода и оставила бы половину истории в старой шкале, а
   половину в новой — различить их потом было бы нечем. */
static int radon_stats_rebase_locked(time_t now) {
    if (!mounted || !is_valid_time(now)) return -1;

    FILE *in = fopen(filename, "r");
    if (!in) return -1;
    const char *tmpname = "/data/radon_rebase.csv";
    FILE *out = fopen(tmpname, "w");
    if (!out) { fclose(in); return -1; }

    int64_t up_now = (int64_t)(esp_timer_get_time() / 1000000);
    char line[128];
    int converted = 0;

    if (fgets(line, sizeof(line), in)) fputs(line, out);   /* заголовок */
    while (fgets(line, sizeof(line), in)) {
        long long ts_raw;
        float r, ra, t, h;
        unsigned w = 1;
        int got = sscanf(line, "%lld,%f,%f,%f,%f,%u", &ts_raw, &r, &ra, &t, &h, &w);
        if (got >= 5 && ts_raw < 0) {
            int64_t age = up_now - (-ts_raw);      /* сколько секунд назад записали */
            long long real = (long long)now - (age > 0 ? age : 0);
            if (got >= 6 && w > 1) {
                fprintf(out, "%lld,%.2f,%.2f,%.1f,%.0f,%u\n", real, r, ra, t, h, w);
            } else {
                fprintf(out, "%lld,%.2f,%.2f,%.1f,%.0f\n", real, r, ra, t, h);
            }
            converted++;
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    if (converted == 0) { remove(tmpname); return 0; }
    remove(filename);
    rename(tmpname, filename);
    radon_stats_bump_generation();   /* #RADEX-172: файл пересобран */
    ESP_LOGI(TAG, "#RADEX-113: переведено записей в реальное время: %d", converted);
    return converted;
}

// Подсчет среднего за период
static bool radon_stats_period_locked(uint32_t seconds_back, radon_period_t *out) {
    if (!mounted) return false;

    FILE *f = fopen(filename, "r");
    if (!f) return false;

    char line[128];
    time_t now = time(NULL);
    time_t start_time = now - seconds_back;
    float sum = 0.0f;
    uint32_t points = 0;
    time_t min_ts = 0;
    time_t max_ts = 0;

    // Пропускаем заголовок
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }

    while (fgets(line, sizeof(line), f)) {
        time_t ts;
        float radon;
        long long ts_raw;
        float _f3, _f4, _f5;
        unsigned w = 1;
        int _got = sscanf(line, "%lld,%f,%f,%f,%f,%u", &ts_raw, &radon, &_f3, &_f4, &_f5, &w);
        if (_got < 6) w = 1;          /* старый формат: одна запись — одно измерение */
        if (_got >= 2) {
            ts = (time_t)ts_raw;
            if (ts >= start_time && is_valid_time(ts)) {
                if (points == 0 || ts < min_ts) min_ts = ts;
                if (points == 0 || ts > max_ts) max_ts = ts;
                sum += radon * (float)w;
                points += w;
            }
        }
    }

    fclose(f);

    if (points == 0) {
        out->mean = 0.0f;
        out->points = 0;
        out->coverage = 0.0f;
        out->span_sec = 0;
        out->valid = false;
        return true;
    }

    out->mean = sum / points;
    out->points = points;
    out->span_sec = max_ts - min_ts;
    /* Покрытие — доля периода, реально охваченная измерениями, считается по
       РАССТОЯНИЮ между первой и последней точкой, а не по их числу.
       Прежняя формула (points * 600 / период) исходила из шага 10 минут, но
       после введения отсева повторов точка пишется лишь когда прибор пересчитал
       значение — раз в 10-60 минут. При цикле 60 мин полностью покрытые сутки
       давали 17 %, и достоверное среднее помечалось недостоверным.
       К охвату добавляем один ожидаемый шаг: последняя точка представляет не
       мгновение, а промежуток до следующей. */
    uint32_t span = (uint32_t)(max_ts - min_ts);
    uint32_t step = points > 1 ? span / (points - 1) : 0;
    out->coverage = seconds_back ? (float)(span + step) / (float)seconds_back : 0.0f;
    if (out->coverage > 1.0f) out->coverage = 1.0f;
    out->valid = points >= 3 && out->coverage >= 0.8;

    return true;
}

/* Прежний интерфейс сохранён: границей служит отметка старта замера. */
void radon_stats_assess(float c_rl, float u_d, bool restricted, radon_assess_t *out) {
    radon_stats_assess_range(radon_stats_test_start(), 0, c_rl, u_d, restricted, out);
}

// Поиск ближайшего меньшего узла в таблице
static void find_uv_kp(int days, bool restricted, float *uv, float *kp) {
    /* Правило выбора значения между узлами таблицы — ШАГОВАЯ функция, не
       интерполяция. Дословно из примечания к Табл. 1 источника: «если
       продолжительность измерения находится между табличными значениями,
       тогда используются более высокие значения UV(t) и Кр(t), например,
       Кр(t = 4,8 суток) = 1,74». Обе величины убывают с ростом t, поэтому
       «более высокое значение» — это значение ближайшего МЕНЬШЕГО узла.

       Идём с КОНЦА таблицы и берём первый узел, не превышающий days. Проход
       с начала (как было в первой генерации) возвращал самый первый узел для
       любой длительности: измерение длиной 100 суток получало неопределённость
       двухсуточного теста, и заключение «соответствует нормативу» становилось
       недостижимым. */
    if (restricted) {
        if (days < restricted_ventilation[0].days) {   /* короче 2 суток */
            *uv = -1.0f; *kp = -1.0f; return;
        }
        for (int i = (int)(sizeof(restricted_ventilation)/sizeof(restricted_ventilation[0])) - 1;
             i >= 0; i--) {
            if (days >= restricted_ventilation[i].days) {
                *uv = restricted_ventilation[i].uv;
                *kp = -1.0f;    /* Kp в ограниченном режиме не определён (§4.2.2) */
                return;
            }
        }
    } else {
        if (days < normal_ventilation[0].days) {       /* короче 4 суток */
            *uv = -1.0f; *kp = -1.0f; return;
        }
        for (int i = (int)(sizeof(normal_ventilation)/sizeof(normal_ventilation[0])) - 1;
             i >= 0; i--) {
            if (days >= normal_ventilation[i].days) {
                *uv = normal_ventilation[i].uv;
                *kp = normal_ventilation[i].kp;
                return;
            }
        }
    }
    *uv = -1.0f; *kp = -1.0f;   /* сюда попасть нельзя: границы проверены выше */
}

// Оценка соответствия по методике Цапалова
/* #RADEX-40: оценка считается либо от отметки старта замера (прежнее
   поведение), либо по ВЫБРАННОМУ интервалу истории — оператор: «рациональный
   метод должен запускаться как прямым измерением, так и чтением данных из
   истории или сохранений». Границы нулевые означают «без ограничения». */
static void radon_stats_assess_range_locked(time_t from, time_t to,
                              float c_rl, float u_d, bool restricted,
                              radon_assess_t *out) {
    /* Структура обнуляется ПЕРВЫМ делом, а входные условия проставляются сразу:
       раньше при раннем выходе («измерений мало») поля оставались с мусором из
       стека, и наружу уходили days=1070474368, c_rl=0 — на живой плате 29.08. */
    memset(out, 0, sizeof(*out));
    out->verdict    = RADON_VERDICT_TOO_SHORT;
    out->c_rl       = c_rl;
    out->restricted = restricted;
    out->kp         = -1.0f;

    if (!mounted) {
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    // Читаем весь файл один раз для всех периодов
    FILE *f = fopen(filename, "r");
    if (!f) {
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    char line[128];
    time_t now = time(NULL);
    time_t first_ts = 0;
    time_t last_ts = 0;
    float sum = 0.0f;
    uint32_t points = 0;

    // Пропускаем заголовок
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    /* #RADEX-16: если человек отметил начало теста, оценка считается ОТ НЕГО.
       Методика оценивает тест — измерение с известным началом. Данные, снятые
       до старта (другая комната, другой режим проветривания, прошлое
       обследование), к текущему тесту не относятся и исказили бы заключение.
       Метки нет — прежнее поведение: считаем по всей истории. */
    time_t test_start = from;

    while (fgets(line, sizeof(line), f)) {
        time_t ts;
        float radon;
        long long ts_raw;
        float _f3, _f4, _f5;
        unsigned w = 1;
        int _got = sscanf(line, "%lld,%f,%f,%f,%f,%u", &ts_raw, &radon, &_f3, &_f4, &_f5, &w);
        if (_got < 6) w = 1;          /* старый формат: одна запись — одно измерение */
        if (_got >= 2) {
            ts = (time_t)ts_raw;
            if (test_start != 0 && ts < test_start) continue;   /* до начала интервала */
            if (to != 0 && ts > to) continue;                   /* после конца интервала */
            if (is_valid_time(ts)) {
                if (points == 0 || ts < first_ts) first_ts = ts;
                if (points == 0 || ts > last_ts) last_ts = ts;
                sum += radon * (float)w;
                points += w;
            }
        }
    }

    fclose(f);

    if (points == 0) {
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    // Вычисляем среднее за весь период
    float c = sum / points;

    // Определяем длительность измерений в сутках
    int days = (int)((last_ts - (test_start != 0 ? test_start : first_ts)) / (24 * 3600));
    if (days < 1) days = 1;

    /* Среднее и длительность отдаём ВСЕГДА, даже когда до оценки не дотянули:
       интерфейсу надо показать «замер идёт N суток, среднее X», иначе на
       вкладке пусто и непонятно, работает ли прибор вообще. */
    out->c    = c;
    out->days = (uint32_t) days;

    // Минимальная длительность теста: 2 суток в ограниченном режиме, 4 в обычном
    if (days < 2) {
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    // Исправляем u_d, если слишком маленькое
    if (u_d < 0.2f) {
        ESP_LOGW(TAG, "Инструментальная неопределённость u_d=%f слишком мала, поднята до 0.2", u_d);
        u_d = 0.2f;
    }
    /* #RADEX-91: клампнутое значение раньше терялось — жило только в этой
       локальной переменной параметра. Сохраняем в структуру СРАЗУ после
       клампа, до любых последующих ранних return (uv<0 и т.п.) — тогда поле
       верно даже для verdict=too_short по причине КОРОТКОЙ таблицы uv/kp, а
       не потому что u_d ещё не применялся. */
    out->u_d_eff = u_d;

    // Находим UV и KP по таблице
    float uv = 0.0f;
    float kp = 0.0f;
    find_uv_kp(days, restricted, &uv, &kp);

    if (uv < 0) {
        out->verdict = RADON_VERDICT_TOO_SHORT;
        return;
    }

    // Критерий (1)
    float crit1 = c * (1.0f + sqrt(uv * uv + u_d * u_d));
    bool comply = crit1 < c_rl;

    // Критерий (2)
    float crit2 = 0.0f;
    bool exceed = false;
    if (kp > 0) {
        crit2 = (c / kp) * (1.0f - u_d);
        exceed = crit2 > c_rl;
    }

    // Принимаем решение
    if (comply) {
        out->verdict = RADON_VERDICT_COMPLIES;
    } else if (exceed || days >= 270) {
        out->verdict = RADON_VERDICT_EXCEEDS;
    } else {
        out->verdict = RADON_VERDICT_UNCERTAIN;
    }

    // Заполняем структуру
    out->c = c;
    out->uv = uv;
    out->kp = kp;
    out->crit1 = crit1;
    out->crit2 = crit2;
    out->days = days;
    out->c_rl = c_rl;
    out->restricted = restricted;
}

// Формирование JSON-ответа
static int radon_stats_json_locked(char *buf, size_t len, float c_rl, float u_d, bool restricted) {
    if (!mounted) {
        /* #RADEX-91: u_d_eff здесь равен НЕклампнутому u_d — до этой точки
           никакая оценка не считалась, поэтому "фактически применённого"
           значения ещё нет; отдаём то же число, чтобы фронтенд не увидел
           ложного расхождения (badge гейтится именно на разницу). */
        return snprintf(buf, len,
            "{\"periods\":{\"d1\":{},\"d7\":{},\"d30\":{},\"d365\":{},\"all\":{}},"
            "\"assess\":{\"verdict\":\"too_short\",\"c\":0.0,\"uv\":0.0,\"kp\":0.0,"
            "\"crit1\":0.0,\"crit2\":0.0,\"days\":0,\"c_rl\":%.1f,\"u_d\":%.2f,\"u_d_eff\":%.2f,\"restricted\":%s},"
            "\"storage\":{\"ok\":false,\"bytes\":0,\"points\":0}}",
            c_rl, u_d, u_d, restricted ? "true" : "false");
    }

    radon_period_t p1, p7, p30, p365, pall;
    radon_stats_period(24*3600, &p1);
    radon_stats_period(7*24*3600, &p7);
    radon_stats_period(30*24*3600, &p30);
    radon_stats_period(365*24*3600, &p365);

    // Читаем весь файл для общего периода
    FILE *f = fopen(filename, "r");
    if (!f) {
        return snprintf(buf, len,
            "{\"periods\":{\"d1\":{},\"d7\":{},\"d30\":{},\"d365\":{},\"all\":{}},"
            "\"assess\":{\"verdict\":\"too_short\",\"c\":0.0,\"uv\":0.0,\"kp\":0.0,"
            "\"crit1\":0.0,\"crit2\":0.0,\"days\":0,\"c_rl\":%.1f,\"u_d\":%.2f,\"u_d_eff\":%.2f,\"restricted\":%s},"
            "\"storage\":{\"ok\":true,\"bytes\":0,\"points\":0}}",
            c_rl, u_d, u_d, restricted ? "true" : "false");
    }

    char line[128];
    time_t first_ts = 0;
    time_t last_ts = 0;
    float sum = 0.0f;
    uint32_t points = 0;

    // Пропускаем заголовок
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return snprintf(buf, len,
            "{\"periods\":{\"d1\":{},\"d7\":{},\"d30\":{},\"d365\":{},\"all\":{}},"
            "\"assess\":{\"verdict\":\"too_short\",\"c\":0.0,\"uv\":0.0,\"kp\":0.0,"
            "\"crit1\":0.0,\"crit2\":0.0,\"days\":0,\"c_rl\":%.1f,\"u_d\":%.2f,\"u_d_eff\":%.2f,\"restricted\":%s},"
            "\"storage\":{\"ok\":true,\"bytes\":0,\"points\":0}}",
            c_rl, u_d, u_d, restricted ? "true" : "false");
    }

    while (fgets(line, sizeof(line), f)) {
        time_t ts;
        float radon;
        long long ts_raw;
        float _f3, _f4, _f5;
        unsigned w = 1;
        int _got = sscanf(line, "%lld,%f,%f,%f,%f,%u", &ts_raw, &radon, &_f3, &_f4, &_f5, &w);
        if (_got < 6) w = 1;          /* старый формат: одна запись — одно измерение */
        if (_got >= 2) {
            ts = (time_t)ts_raw;
            if (is_valid_time(ts)) {
                if (points == 0 || ts < first_ts) first_ts = ts;
                if (points == 0 || ts > last_ts) last_ts = ts;
                sum += radon * (float)w;
                points += w;
            }
        }
    }

    fclose(f);

    if (points > 0) {
        pall.mean = sum / points;
        pall.points = points;
        pall.span_sec = last_ts - first_ts;
        /* Для «всего времени» доля покрытия бессмысленна: период РАВЕН охвату
           по определению, отношение всегда единица. Прежняя формула
           (points * 600 / span) — та самая, что забракована для остальных
           периодов (#RADEX-32): исходит из шага 10 минут, которого после
           отсева повторов нет. Достоверность здесь определяется числом точек
           и длительностью наблюдения, а не долей. */
        pall.coverage = 1.0f;
        pall.valid = points >= 3;
    } else {
        pall.mean = 0.0f;
        pall.points = 0;
        pall.span_sec = 0;
        pall.coverage = 0.0f;
        pall.valid = false;
    }

    // Статистика хранилища
    struct stat st;
    int bytes = 0;
    if (stat(filename, &st) == 0) {
        bytes = st.st_size;
    }

    radon_assess_t assess;
    radon_stats_assess(c_rl, u_d, restricted, &assess);

    /* #RADEX-87: один вызов вместо двух. Функция открывает NVS, и звать её
       дважды подряд ради значения и флага «не ноль» — это два открытия
       раздела на каждый запрос /api/stats. */
    /* #RADEX-150: наружу отдаём ЭФФЕКТИВНОЕ начало (явная отметка либо первая
       запись истории), а рядом — было ли оно отмечено человеком. Без второго
       поля страница не смогла бы отличить «замер начат кнопкой такого-то
       числа» от «идёт по всей накопленной истории», и надпись про начало была
       бы утверждением, которого никто не делал (#AH-1). */
    time_t tstart = radon_stats_test_start_eff();
    bool   texplicit = radon_stats_test_start() > 0;

    // Формируем JSON
    return snprintf(buf, len,
        "{\"periods\":{\"d1\":{\"mean\":%.1f,\"points\":%u,\"coverage\":%.2f,\"span\":%u,\"valid\":%s},"
        "\"d7\":{\"mean\":%.1f,\"points\":%u,\"coverage\":%.2f,\"span\":%u,\"valid\":%s},"
        "\"d30\":{\"mean\":%.1f,\"points\":%u,\"coverage\":%.2f,\"span\":%u,\"valid\":%s},"
        "\"d365\":{\"mean\":%.1f,\"points\":%u,\"coverage\":%.2f,\"span\":%u,\"valid\":%s},"
        "\"all\":{\"mean\":%.1f,\"points\":%u,\"coverage\":%.2f,\"span\":%u,\"valid\":%s}},"
        "\"assess\":{\"verdict\":\"%s\",\"c\":%.1f,\"uv\":%.2f,\"kp\":%.2f,"
        /* #RADEX-91: u_d — то, что ввёл пользователь (эхо запроса, как и
           раньше); u_d_eff — то, что реально вошло в crit1/crit2 после
           поднятия до минимума методики (0.2). Расходятся только когда
           введённое значение было МЕНЬШЕ минимума — иначе равны, и страница
           молчит (см. methodTick()). */
        "\"crit1\":%.1f,\"crit2\":%.1f,\"days\":%u,\"c_rl\":%.1f,\"u_d\":%.2f,\"u_d_eff\":%.2f,\"restricted\":%s},"
        "\"storage\":{\"ok\":true,\"bytes\":%d,\"points\":%u},"
        "\"test\":{\"start\":%lld,\"started\":%s,\"explicit\":%s}}",
        p1.mean, (unsigned)p1.points, p1.coverage, (unsigned)p1.span_sec, p1.valid ? "true" : "false",
        p7.mean, (unsigned)p7.points, p7.coverage, (unsigned)p7.span_sec, p7.valid ? "true" : "false",
        p30.mean, (unsigned)p30.points, p30.coverage, (unsigned)p30.span_sec, p30.valid ? "true" : "false",
        p365.mean, (unsigned)p365.points, p365.coverage, (unsigned)p365.span_sec, p365.valid ? "true" : "false",
        pall.mean, (unsigned)pall.points, pall.coverage, (unsigned)pall.span_sec, pall.valid ? "true" : "false",
        assess.verdict == RADON_VERDICT_TOO_SHORT ? "too_short" :
        assess.verdict == RADON_VERDICT_COMPLIES ? "complies" :
        assess.verdict == RADON_VERDICT_EXCEEDS ? "exceeds" : "uncertain",
        assess.c, assess.uv, assess.kp, assess.crit1, assess.crit2,
        (unsigned)assess.days, assess.c_rl, u_d,
        assess.u_d_eff > 0 ? assess.u_d_eff : u_d,   /* оценка не дошла до клампа — эхо введённого, расхождения нет */
        assess.restricted ? "true" : "false", (int)bytes, (unsigned)points,
        (long long) tstart,
        tstart != 0 ? "true" : "false",
        texplicit ? "true" : "false");
}

// Путь к файлу истории — нужен веб-серверу, чтобы отдать его на скачивание
// целиком, не пересобирая содержимое.
const char *radon_stats_file(void) { return filename; }

static bool radon_stats_start_test_locked(void) {
    time_t now = time(NULL);
    if (!is_valid_time(now)) {
        ESP_LOGE(TAG, "Cannot start test: system time is not synchronized");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("radon", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for test start");
        return false;
    }

    err = nvs_set_i64(handle, "tstart", now);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save test start time to NVS");
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit test start time to NVS");
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "начало теста отмечено: %lld", (long long) now);

    /* #RADEX-102: создаём файл ЭТОГО замера. Общий /data/radon.csv продолжает
       копить всё как раньше — новый файл не заменяет его, а дублирует срез.
       Неудачу создания НЕ считаем провалом старта: NVS-отметка уже сохранена,
       заключение по методике по-прежнему читает общий файл с фильтром
       from=tstart и работает без файла замера — тот нужен только для защиты
       от «Сброса» и списка на «Графиках». */
    char tf[48];
    radon_stats_test_file(now, tf, sizeof(tf));
    if (tf[0]) {
        FILE *tfp = fopen(tf, "w");
        if (tfp) {
            fprintf(tfp, "time,radon,radon_avg,temp,hum\n");
            fclose(tfp);
        } else {
            ESP_LOGW(TAG, "не удалось создать файл замера %s (продолжаем без него)", tf);
        }
    }
    return true;
}

static bool radon_stats_reset_locked(void) {
    /* #RADEX-102, оператор: "каждый новый замер должен создавать отдельный
       файл на флеш". Раньше "Сброс" удалял ОБЩИЙ /data/radon.csv целиком -
       вместе с текущим замером стиралась и вся история для "Показаний" и
       "Графиков" (подтверждение честно предупреждало об этом, но само
       поведение было архитектурно грубым: перезапустить замер без потери
       ВСЕЙ истории было нельзя). Теперь удаляется ТОЛЬКО файл текущего
       замера; общий файл истории не трогается вообще. */
    if (!mounted) {
        ESP_LOGE(TAG, "Cannot reset: filesystem not mounted");
        return false;
    }

    time_t start = radon_stats_test_start();
    if (start > 0) {
        char tf[48];
        radon_stats_test_file(start, tf, sizeof(tf));
        radon_stats_bump_generation();   /* #RADEX-172: файл замера исчезает */
        if (tf[0] && remove(tf) != 0) {
            /* Файла может не быть, если start_test() не смог его создать
               (см. её собственный ESP_LOGW) - это не повод отказывать в
               сбросе, метку NVS всё равно нужно снять. */
            ESP_LOGW(TAG, "файл замера %s не удалён (возможно, отсутствовал)", tf);
        }
    }

    // Стираем метку теста из NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("radon", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for test reset");
        return false;
    }

    err = nvs_erase_key(handle, "tstart");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase test start key from NVS");
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit test reset in NVS");
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Файл замера удалён, отметка теста снята (общая история не тронута)");
    return true;
}

static time_t radon_stats_test_start_locked(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("radon", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* #RADEX-87. На чистой плате раздела "radon" ещё нет — это НОРМАЛЬНОЕ
           состояние «замер не начат», а не сбой. Прежде оно печаталось уровнем
           E и по шесть строк в минуту (функция зовётся трижды на запрос), и
           первое включение выглядело аварийным. Настоящую ошибку открытия —
           повреждённый раздел, нехватку места — по-прежнему показываем. */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "не удалось открыть NVS для чтения начала замера: %s",
                     esp_err_to_name(err));
        }
        return 0;
    }

    int64_t start_time;
    err = nvs_get_i64(handle, "tstart", &start_time);
    nvs_close(handle);

    if (err != ESP_OK) {
        return 0;
    }

    if (!is_valid_time((time_t)start_time)) {
        return 0;
    }

    return (time_t)start_time;
}

/* #RADEX-150: обоснование — в radon_stats.h. Механика: явная отметка сильнее,
   иначе первая ВАЛИДНАЯ по времени запись истории (относительные метки до
   синхронизации часов, #RADEX-113, пропускаем — датировать замер нечем). */
static time_t radon_stats_test_start_eff_locked(void)
{
    time_t s = radon_stats_test_start();
    if (s > 0) return s;
    if (!mounted) return 0;
    FILE *f = fopen(filename, "r");
    if (!f) return 0;
    char line[128]; long long ts_raw; float r; time_t first = 0;
    if (fgets(line, sizeof(line), f)) {          /* заголовок */
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lld,%f", &ts_raw, &r) >= 2 && is_valid_time((time_t)ts_raw)) {
                first = (time_t)ts_raw; break;
            }
        }
    }
    fclose(f);
    return first;
}

/* #RADEX-102: путь вычисляется детерминированно из start — отдельного поля в
   NVS не заводим, оно избыточно, раз tstart уже хранится и start=0 однозначно
   означает «замер не начат». */
void radon_stats_test_file(time_t start, char *out, size_t out_sz)
{
    if (start <= 0 || out_sz < 12) { out[0] = 0; return; }
    snprintf(out, out_sz, "/data/test_%lld.csv", (long long)start);
}

/* #RADEX-102: список сохранённых замеров с флеш — обход /data за файлами
   вида test_<epoch>.csv (имя строит radon_stats_test_file(), здесь разбираем
   его в обратную сторону). points/span_sec считаются по СОДЕРЖИМОМУ файла,
   не по NVS: список остаётся верным, даже если метка активного замера уже
   снята «Сбросом» (тот удаляет файл вместе с меткой, но архивные файлы
   прошлых замеров NVS не касаются вовсе). */
static int radon_stats_tests_list_json_locked(char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    if (!mounted) {
        int n = snprintf(buf, len, "[]");
        return (n < 0 || (size_t)n >= len) ? -1 : n;
    }

    DIR *d = opendir("/data");
    if (!d) {
        int n = snprintf(buf, len, "[]");
        return (n < 0 || (size_t)n >= len) ? -1 : n;
    }

    time_t active = radon_stats_test_start();
    int written = snprintf(buf, len, "[");
    if (written < 0 || (size_t)written >= len) { closedir(d); return -1; }
    size_t pos = (size_t)written;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        long long start_epoch;
        int consumed = 0;
        /* "%n" проверяет, что после числа сразу идёт ровно ".csv" и больше
           ничего — иначе "test_123abc.csv" тоже совпало бы с "%lld". */
        if (sscanf(e->d_name, "test_%lld.csv%n", &start_epoch, &consumed) != 1) continue;
        if (consumed != (int)strlen(e->d_name)) continue;
        if (start_epoch <= 0) continue;

        /* Буфер по размеру d_name + "/data/": GCC (-Werror=format-truncation)
           считает возможным d_name длиной до 255 (тип поля, не факт SPIFFS) и
           роняет сборку на 64-байтном буфере, даже если реальные имена
           короче в разы. */
        char path[8 + sizeof(e->d_name)];
        snprintf(path, sizeof(path), "/data/%s", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;   /* попался в листинге, но не открылся — пропускаем */
        char line[128];
        if (!fgets(line, sizeof(line), f)) { fclose(f); continue; }   /* пустой файл, даже заголовка нет */

        uint32_t points = 0;
        time_t first_ts = 0, last_ts = 0;
        long long ts; float r, ra, t, h;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lld,%f,%f,%f,%f", &ts, &r, &ra, &t, &h) == 5) {
                if (points == 0 || ts < first_ts) first_ts = ts;
                if (points == 0 || ts > last_ts) last_ts = ts;
                points++;
            }
        }
        fclose(f);
        uint32_t span = (points > 0) ? (uint32_t)(last_ts - first_ts) : 0;
        bool is_active = (active > 0 && (time_t)start_epoch == active);

        written = snprintf(buf + pos, len - pos,
            "%s{\"start\":%lld,\"points\":%u,\"span_sec\":%u,\"active\":%s}",
            count > 0 ? "," : "", start_epoch, (unsigned)points, (unsigned)span,
            is_active ? "true" : "false");
        if (written < 0 || (size_t)written >= len - pos) { closedir(d); return -1; }
        pos += (size_t)written;
        count++;
    }
    closedir(d);

    written = snprintf(buf + pos, len - pos, "]");
    if (written < 0 || (size_t)written >= len - pos) return -1;
    pos += (size_t)written;
    return (int)pos;
}

/* Внутренний предел числа точек в ответе за один замер. У функции нет
   параметра max (сигнатура фиксирована в radon_stats.h) — окну графика
   больше и не нужно: /api/history/range по умолчанию отдаёт до 1000. */
#define RADON_TEST_POINTS_MAX 1000

/* #RADEX-102: точки ОДНОГО файла замера — тот же формат JSON, что отдаёт
   /api/history/range (main/web_server.c, handle_history_range), чтобы
   страница переиспользовала уже написанный парсер точек. Источник — файл
   замера (radon_stats_test_file), не общий /data/radon.csv. */
static int radon_stats_test_points_json_locked(time_t start, char *buf, size_t len) {
    if (!buf || len == 0) return -1;
    char tf[48];
    radon_stats_test_file(start, tf, sizeof(tf));
    if (!tf[0]) return -1;
    FILE *f = fopen(tf, "r");
    if (!f) return -1;   /* файла с таким start не существует */

    char line[128];
    if (!fgets(line, sizeof(line), f)) {   /* файл есть, но даже заголовка нет */
        fclose(f);
        int n = snprintf(buf, len, "{\"n\":0,\"total\":0,\"step\":1,\"points\":[]}");
        return (n < 0 || (size_t)n >= len) ? -1 : n;
    }

    long total = 0;
    long long ts; float radon, radon_avg, temp, hum;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lld,%f,%f,%f,%f", &ts, &radon, &radon_avg, &temp, &hum) == 5) total++;
    }
    fclose(f);
    if (total == 0) {
        int n = snprintf(buf, len, "{\"n\":0,\"total\":0,\"step\":1,\"points\":[]}");
        return (n < 0 || (size_t)n >= len) ? -1 : n;
    }

    int step = 1;
    if (total > RADON_TEST_POINTS_MAX) {
        step = (int)(total / RADON_TEST_POINTS_MAX);
        if (step < 1) step = 1;
    }

    f = fopen(tf, "r");
    if (!f) return -1;
    fgets(line, sizeof(line), f);   /* заголовок пропускаем повторно */

    int written = snprintf(buf, len, "{\"total\":%ld,\"step\":%d,\"points\":[", total, step);
    if (written < 0 || (size_t)written >= len) { fclose(f); return -1; }
    size_t pos = (size_t)written;
    long count = 0;
    int sent = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lld,%f,%f,%f,%f", &ts, &radon, &radon_avg, &temp, &hum) != 5) continue;

        bool keep = (count == 0) || (count == total - 1) ||
                    (total <= RADON_TEST_POINTS_MAX) || (count % step == 0);
        if (!keep) { count++; continue; }

        float r_f = radon, c_f = temp, h_f = hum;
        if (isnan(radon)) r_f = -1.0f;
        if (isnan(temp))  c_f = -100.0f;
        if (isnan(hum) || hum == 255.0f) h_f = -1.0f;
        char rs[16], cs[16], hs[16];
        if (r_f < 0 || isnan(r_f)) snprintf(rs, sizeof(rs), "null"); else snprintf(rs, sizeof(rs), "%.2f", r_f);
        if (c_f < -90 || isnan(c_f)) snprintf(cs, sizeof(cs), "null"); else snprintf(cs, sizeof(cs), "%.1f", c_f);
        if (h_f < 0 || h_f > 100 || isnan(h_f)) snprintf(hs, sizeof(hs), "null"); else snprintf(hs, sizeof(hs), "%.0f", h_f);

        written = snprintf(buf + pos, len - pos, "%s{\"t\":%lld,\"r\":%s,\"c\":%s,\"h\":%s}",
                            sent > 0 ? "," : "", ts, rs, cs, hs);
        if (written < 0 || (size_t)written >= len - pos) { fclose(f); return -1; }
        pos += (size_t)written;
        sent++;
        count++;
    }
    fclose(f);

    written = snprintf(buf + pos, len - pos, "],\"n\":%d}", sent);
    if (written < 0 || (size_t)written >= len - pos) return -1;
    pos += (size_t)written;
    return (int)pos;
}

/* Последнее записанное значение радона — нужно после перезагрузки, чтобы
   не записать повтор как новое измерение. Состояние отсева живёт в ОЗУ и
   перезагрузку не переживает; на живых данных 29.08 это дало шесть дублей
   одного значения за час перепрошивок. Читаем хвост файла, а не весь файл:
   он до 2 МБ. */
static float radon_stats_last_radon_locked(void)
{
    if (!mounted) return NAN;
    FILE *f = fopen(filename, "r");
    if (!f) return NAN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NAN; }
    long size = ftell(f);
    long back = size < 256 ? size : 256;         /* хвоста хватит на пару строк */
    if (fseek(f, size - back, SEEK_SET) != 0) { fclose(f); return NAN; }
    char buf[300] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return NAN;
    buf[n] = 0;
    char *last = NULL, *p2 = strtok(buf, "\n");
    while (p2) { last = p2; p2 = strtok(NULL, "\n"); }
    if (!last) return NAN;
    long long ts; float radon;
    if (sscanf(last, "%lld,%f", &ts, &radon) != 2) return NAN;
    return radon;
}



// Вспомогательная функция для записи накопленной группы
static void write_group(FILE *fout, long long grp_key, long long grp_from, long long grp_to,
                        double sum_r, double sum_ravg, unsigned sum_n,
                        double sum_t, double sum_h, unsigned cnt_t, unsigned cnt_h) {
    // Среднее время — середина интервала
    long long ts = grp_from + (grp_to - grp_from) / 2;
    // Взвешенное среднее по n
    float radon = (sum_n > 0) ? (float)(sum_r / sum_n) : 0.0f;
    float radon_avg = (sum_n > 0) ? (float)(sum_ravg / sum_n) : 0.0f;
    float temp = (cnt_t > 0) ? (float)(sum_t / cnt_t) : NAN;
    float hum = (cnt_h > 0) ? (float)(sum_h / cnt_h) : 255.0f;
    // Если radon == 0, то строку пропускаем
    if (isnan(radon) || radon == 0.0f) return;
    fprintf(fout, "%lld,%.2f,%.2f,%.1f,%.0f,%u\n", ts, radon, radon_avg, temp, hum, sum_n);
}

static int radon_stats_compact_locked(uint32_t raw_days, uint32_t hour_days) {
    // По умолчанию: 30 суток — как есть, 365 — среднечасовые
    /* Ноль здесь означает «не хранить сырых суток вовсе», а не «взять умолчание»:
       подмена нуля делала невозможной свёртку всей истории — и проверку функции
       на коротком ряду тоже. Умолчания (30 и 365) задаёт вызывающий обработчик. */
    if (hour_days < raw_days) hour_days = raw_days;

    /* #RADEX-173 (найдено 01.09.2026 ценой реальных данных). Здесь стоял
       ЗАШИТЫЙ "/data/radon.csv" вместо filename — единственное место в модуле,
       которое не спрашивало, с каким файлом он работает. В обычной сборке пути
       совпадают, поэтому дефект не проявлялся никогда. Проявился, когда модуль
       перевели на отдельный файл для нагрузочного стенда: все чтения шли в
       stress.csv, а уплотнение молча пересобрало НАСТОЯЩУЮ историю оператора —
       71 сырая запись свернулась в 5 суточных средних. Сырьё уцелело только
       потому, что было выгружено заранее.
       Урок класса: у модуля должен быть ОДИН источник имени файла. Любая
       вторая копия пути — это место, где он однажды разойдётся. */
    const char *fname_in = filename;
    const char *fname_out = "/data/radon_c.csv";

    FILE *fin = fopen(fname_in, "r");
    if (!fin) {
        ESP_LOGE(TAG, "Не удалось открыть входной файл %s", fname_in);
        return -1;
    }

    FILE *fout = fopen(fname_out, "w");
    if (!fout) {
        ESP_LOGE(TAG, "Не удалось открыть выходной файл %s", fname_out);
        fclose(fin);
        return -1;
    }

    // Читаем заголовок
    char line[128];
    if (fgets(line, sizeof(line), fin) == NULL) {
        fclose(fin);
        fclose(fout);
        remove(fname_out);
        return -1;
    }
    fputs(line, fout);  // Записываем заголовок

    time_t now = time(NULL);
    long long grp_key = -1;      /* ключ текущей группы: ts/3600 либо ts/86400 */
    long long grp_from = 0, grp_to = 0;   /* границы времени группы, для середины метки */
    double sum_r = 0, sum_ravg = 0;       /* взвешенные суммы */
    unsigned sum_n = 0;                   /* суммарное число исходных измерений */
    double sum_t = 0, sum_h = 0;          /* температура и влажность — простым средним */
    unsigned cnt_t = 0, cnt_h = 0;        /* сколько строк группы их реально измерили */
    int mode = 0;                         /* 0 — как есть, 1 — час, 2 — сутки */

    long long total_in = 0;
    long long total_out = 0;

    while (fgets(line, sizeof(line), fin)) {
        total_in++;
        long long ts;
        float radon, radon_avg, temp, hum;
        unsigned n;
        /* Вес ОБЯЗАН быть проинициализирован до разбора: в строке старого формата
           шестого поля нет, sscanf переменную не трогает, и в неё попадал мусор —
           на живых данных это дало вес 3 169 380 492 и суммы вместо средних. */
        n = 1;
        int fields = sscanf(line, "%lld,%f,%f,%f,%f,%u", &ts, &radon, &radon_avg, &temp, &hum, &n);
        if (fields < 6) n = 1;
        if (fields < 5) {
            // Старый формат — считаем n = 1
            n = 1;
            fields = sscanf(line, "%lld,%f,%f,%f,%f", &ts, &radon, &radon_avg, &temp, &hum);
        }
        if (fields < 5) continue;  // Пропускаем некорректные строки

        int age_days = (now - ts) / 86400;
        int new_mode = 0;
        if (age_days < raw_days) {
            new_mode = 0;  // Как есть
        } else if (age_days < hour_days) {
            new_mode = 1;  // Среднечасовые
        } else {
            new_mode = 2;  // Среднесуточные
        }

        long long key;
        if (new_mode == 0) {
            // Просто записываем как есть
            fprintf(fout, "%s", line);
            total_out++;
            continue;
        } else if (new_mode == 1) {
            key = ts / 3600;
        } else {
            key = ts / 86400;
        }

        // Если сменился ключ или режим — записываем накопленную группу
        if (grp_key != -1 && (grp_key != key || mode != new_mode)) {
            write_group(fout, grp_key, grp_from, grp_to, sum_r, sum_ravg, sum_n,
                       sum_t, sum_h, cnt_t, cnt_h);
            total_out++;
        }

        // Начинаем новую группу или продолжаем текущую
        if (grp_key == -1 || grp_key != key || mode != new_mode) {
            grp_key = key;
            grp_from = (new_mode == 1) ? (key * 3600) : (key * 86400);
            grp_to = (new_mode == 1) ? ((key + 1) * 3600) : ((key + 1) * 86400);
            sum_r = radon * n;
            sum_ravg = radon_avg * n;
            sum_n = n;
            sum_t = temp;
            sum_h = hum;
            cnt_t = isnan(temp) ? 0 : 1;
            cnt_h = isnan(hum) ? 0 : 1;
        } else {
            // Продолжаем накапливать
            sum_r += radon * n;
            sum_ravg += radon_avg * n;
            sum_n += n;
            if (!isnan(temp)) {
                sum_t += temp;
                cnt_t++;
            }
            if (!isnan(hum)) {
                sum_h += hum;
                cnt_h++;
            }
        }

        mode = new_mode;
    }

    // Записываем последнюю группу
    if (grp_key != -1) {
        write_group(fout, grp_key, grp_from, grp_to, sum_r, sum_ravg, sum_n,
                   sum_t, sum_h, cnt_t, cnt_h);
        total_out++;
    }

    fclose(fin);
    fclose(fout);

    // Заменяем оригинальный файл
    remove(fname_in);
    if (rename(fname_out, fname_in) != 0) {
        ESP_LOGE(TAG, "Ошибка переименования файла");
        radon_stats_bump_generation();   /* #RADEX-172: файла на месте уже нет */
        return -1;
    }
    radon_stats_bump_generation();       /* #RADEX-172: файл пересобран */

    ESP_LOGI(TAG, "Уплотнение завершено: было %lld строк, стало %lld", total_in, total_out);
    return (int)total_out;
}

/* #RADEX-40: заключение по выбранному интервалу истории. Отдельный ответ, а не
   расширение /api/stats: там оценка идёт от отметки старта замера, и смешивать
   два смысла в одном поле — верный способ показать заключение не про тот период,
   который выбрал человек. */
int radon_stats_assess_json(char *buf, size_t len, time_t from, time_t to,
                            float c_rl, float u_d, bool restricted) {
    radon_assess_t a;
    radon_stats_assess_range(from, to, c_rl, u_d, restricted, &a);
    const char *v = a.verdict == RADON_VERDICT_TOO_SHORT ? "too_short" :
                    a.verdict == RADON_VERDICT_COMPLIES  ? "complies"  :
                    a.verdict == RADON_VERDICT_EXCEEDS   ? "exceeds"   : "uncertain";
    /* #RADEX-91: та же пара полей, что в radon_stats_json — u_d (введено),
       u_d_eff (реально применено, после клампа до 0.2). Раньше сюда u_d вообще
       не попадал. */
    return snprintf(buf, len,
        "{\"verdict\":\"%s\",\"c\":%.1f,\"uv\":%.2f,\"kp\":%.2f,"
        "\"crit1\":%.1f,\"crit2\":%.1f,\"days\":%u,\"c_rl\":%.1f,\"u_d\":%.2f,\"u_d_eff\":%.2f,"
        "\"restricted\":%s,\"from\":%lld,\"to\":%lld}",
        v, a.c, a.uv, a.kp, a.crit1, a.crit2, (unsigned)a.days, a.c_rl, u_d,
        a.u_d_eff > 0 ? a.u_d_eff : u_d,
        restricted ? "true" : "false", (long long)from, (long long)to);
}

/* #RADEX-172: Рекурсивный мьютекс модуля + обёртки над публичными функциями */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mtx = NULL;
static uint32_t s_lock_timeouts = 0;

bool radon_stats_mutex_create(void)
{
    if (s_mtx) {
        return true;
    }
    s_mtx = xSemaphoreCreateRecursiveMutex();
    return s_mtx != NULL;
}

bool radon_stats_lock(uint32_t timeout_ms)
{
#ifdef RADEX172_MUTATION_NO_LOCK
    /* #SA-3, МУТАЦИЯ. Не включать в рабочей сборке. Собирается только вручную
       (idf.py -DRADEX172_MUTATION_NO_LOCK=1) и служит одному: показать, что
       нагрузочный стенд scripts/radex172_load_test.py УМЕЕТ КРАСНЕТЬ на своём
       классе дефекта. Мьютекс превращается в пустышку, всё остальное в коде
       остаётся тем же — значит покраснение объясняется снятой блокировкой, а
       не побочной правкой. Зелёный стенд при этой мутации означал бы, что
       стенд не проверяет то, ради чего написан. */
    (void)timeout_ms;
    return true;
#else
    if (!s_mtx) {
        return false;
    }
    if (xSemaphoreTakeRecursive(s_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return true;
    }
    s_lock_timeouts++;
    ESP_LOGE(TAG, "radon_stats_lock timeout %u ms", timeout_ms);
    return false;
#endif
}

void radon_stats_unlock(void)
{
#ifndef RADEX172_MUTATION_NO_LOCK   /* см. мутацию в radon_stats_lock */
    if (s_mtx) {
        xSemaphoreGiveRecursive(s_mtx);
    }
#endif
}

uint32_t radon_stats_lock_timeouts(void)
{
    return s_lock_timeouts;
}

/* #RADEX-172: поколение файла. Увеличивается ТОЛЬКО там, где файл
   пересобирается целиком (см. пояснение в radon_stats.h). Обычная дозапись
   поколение не меняет — она читателю не мешает. */
static uint32_t s_generation;

uint32_t radon_stats_generation(void)
{
    return s_generation;
}

void radon_stats_bump_generation(void)
{
    s_generation++;
}

#define RS_LOCK_MS_WRITE 10000
#define RS_LOCK_MS_READ   5000

void radon_stats_add(time_t ts, float radon, float radon_avg, float temp, float hum)
{
    if (!radon_stats_lock(RS_LOCK_MS_WRITE)) {
        return;
    }
    radon_stats_add_locked(ts, radon, radon_avg, temp, hum);
    radon_stats_unlock();
}

int radon_stats_rebase(time_t now)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return -1;
    }
    int result = radon_stats_rebase_locked(now);
    radon_stats_unlock();
    return result;
}

bool radon_stats_period(uint32_t seconds_back, radon_period_t *out)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    bool result = radon_stats_period_locked(seconds_back, out);
    radon_stats_unlock();
    return result;
}

void radon_stats_assess_range(time_t from, time_t to, float c_rl, float u_d, bool restricted, radon_assess_t *out)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return;
    }
    radon_stats_assess_range_locked(from, to, c_rl, u_d, restricted, out);
    radon_stats_unlock();
}

int radon_stats_json(char *buf, size_t len, float c_rl, float u_d, bool restricted)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return -1;
    }
    int result = radon_stats_json_locked(buf, len, c_rl, u_d, restricted);
    radon_stats_unlock();
    return result;
}

bool radon_stats_start_test(void)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return false;
    }
    bool result = radon_stats_start_test_locked();
    radon_stats_unlock();
    return result;
}

bool radon_stats_reset(void)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return false;
    }
    bool result = radon_stats_reset_locked();
    radon_stats_unlock();
    return result;
}

time_t radon_stats_test_start(void)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return 0;
    }
    time_t result = radon_stats_test_start_locked();
    radon_stats_unlock();
    return result;
}

time_t radon_stats_test_start_eff(void)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return 0;
    }
    time_t result = radon_stats_test_start_eff_locked();
    radon_stats_unlock();
    return result;
}

int radon_stats_tests_list_json(char *buf, size_t len)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return -1;
    }
    int result = radon_stats_tests_list_json_locked(buf, len);
    radon_stats_unlock();
    return result;
}

int radon_stats_test_points_json(time_t start, char *buf, size_t len)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return -1;
    }
    int result = radon_stats_test_points_json_locked(start, buf, len);
    radon_stats_unlock();
    return result;
}

float radon_stats_last_radon(void)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return NAN;
    }
    float result = radon_stats_last_radon_locked();
    radon_stats_unlock();
    return result;
}

int radon_stats_compact(uint32_t raw_days, uint32_t hour_days)
{
    if (!radon_stats_lock(RS_LOCK_MS_READ)) {
        return -1;
    }
    int result = radon_stats_compact_locked(raw_days, hour_days);
    radon_stats_unlock();
    return result;
}
