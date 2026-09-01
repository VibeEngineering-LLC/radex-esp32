#include "log_ring.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// 24 КБ хватает на ~250 строк — это десятки минут работы шлюза. Больше не
// берём: PSRAM есть не на всех платах семейства, а падать из-за лога нельзя.
#define RING_SZ (24 * 1024)

static char             *s_ring;
static size_t            s_head;      // куда писать
static bool              s_wrapped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t    s_prev;

static int log_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0 && s_ring && s_lock &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        for (int i = 0; i < n; i++) {
            s_ring[s_head++] = line[i];
            if (s_head >= RING_SZ) { s_head = 0; s_wrapped = true; }
        }
        xSemaphoreGive(s_lock);
    }
    return s_prev ? s_prev(fmt, ap) : n;   // на UART тоже отдаём
}

void log_ring_init(void)
{
    s_ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM);
    if (!s_ring) s_ring = malloc(RING_SZ);      // без PSRAM — из обычной кучи
    if (!s_ring) return;
    s_lock = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(log_vprintf);
}

int log_ring_dump(char *buf, size_t len)
{
    if (!s_ring || !s_lock || len < 2) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return 0;

    /* Кольцо 24 КБ, а приёмник обычно 8 КБ. Раньше при нехватке места
       отдавалось НАЧАЛО — самые старые записи, и свежих в браузере не было
       видно вовсе: журнал становился бесполезен через несколько минут работы
       (поймано на живой плате 29.08 при разборе отказа связи).
       Теперь при нехватке отдаём ПОСЛЕДНИЕ len-1 байт: диагностируют всегда
       по свежему хвосту. */
    size_t avail = s_wrapped ? RING_SZ : s_head;      /* сколько всего накоплено */
    size_t cap   = len - 1;
    size_t take  = avail < cap ? avail : cap;

    /* Логическая позиция начала выдачи: конец данных минус take. */
    size_t start = s_wrapped ? (s_head + (RING_SZ - take)) % RING_SZ
                             : s_head - take;

    size_t out = 0;
    size_t first = RING_SZ - start;                   /* до конца буфера */
    if (first > take) first = take;
    memcpy(buf, s_ring + start, first);
    out = first;
    if (take > first) {                               /* остаток с начала буфера */
        memcpy(buf + out, s_ring, take - first);
        out += take - first;
    }

    /* Первая строка почти наверняка обрезана посередине — отбрасываем её. */
    if (avail > cap) {
        size_t skip = 0;
        while (skip < out && buf[skip] != 0x0A) skip++;
        if (skip < out) {
            skip++;
            memmove(buf, buf + skip, out - skip);
            out -= skip;
        }
    }

    buf[out] = 0;
    xSemaphoreGive(s_lock);
    return (int) out;
}

/* #RADEX-105: сброс кольца по запросу пользователя. Не освобождает и не
   перевыделяет память — просто возвращает указатели записи в начало, ровно
   как при старте. Обнулять сам буфер незачем: dump() отдаёт только диапазон
   [0, s_head) при !s_wrapped, старое содержимое за головой никогда не читается. */
void log_ring_clear(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_head = 0;
    s_wrapped = false;
    xSemaphoreGive(s_lock);
}

// ── История показаний ─────────────────────────────────────────────────────
static hist_point_t s_hist[HIST_N];
static uint16_t     s_hist_n;      // сколько заполнено (до HIST_N)
static uint16_t     s_hist_head;   // куда писать следующую

void log_ring_hist_push(float radon, float temp, float hum)
{
    hist_point_t *p = &s_hist[s_hist_head];
    p->radon = radon;
    p->temp  = temp;
    /* 255 — маркер «не измерено»: приведение NAN к uint8_t даёт мусор,
       а ноль читался бы как измеренная нулевая влажность. */
    p->hum   = isnan(hum) ? 255 : (uint8_t) hum;
    p->t_sec = (uint32_t) (esp_timer_get_time() / 1000000);
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_n < HIST_N) s_hist_n++;
}

int log_ring_hist_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, "{\"n\":%u,\"points\":[", s_hist_n);
    // Отдаём в хронологическом порядке: график рисуется слева направо.
    uint16_t start = (s_hist_n == HIST_N) ? s_hist_head : 0;
    for (uint16_t i = 0; i < s_hist_n && n > 0 && n < (int)len; i++) {
        hist_point_t *p = &s_hist[(start + i) % HIST_N];
        /* Неизмеренные величины отдаём как null, а не числом: с ротацией
           характеристик (прибор отдаёт лишь 4 значения за сессию) температура
           и влажность приходят не в каждом круге. Печать NAN как %.1f давала
           литерал nan — невалидный JSON, страница не разбирала ответ вовсе;
           влажность 255 — маркер «не измерено», а не 255 %. */
        char c_s[12], h_s[12];
        if (isnan(p->temp)) snprintf(c_s, sizeof(c_s), "null");
        else                snprintf(c_s, sizeof(c_s), "%.1f", p->temp);
        if (p->hum == 255)  snprintf(h_s, sizeof(h_s), "null");
        else                snprintf(h_s, sizeof(h_s), "%u", p->hum);
        n += snprintf(buf + n, len - n, "%s{\"t\":%lu,\"r\":%.1f,\"c\":%s,\"h\":%s}",
                      i ? "," : "", (unsigned long) p->t_sec, p->radon, c_s, h_s);
    }
    if (n > 0 && n < (int)len) n += snprintf(buf + n, len - n, "]}");
    return n;
}
