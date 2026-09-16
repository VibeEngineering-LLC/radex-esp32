// #RADEX-281 (аудит 281-B F2/F3/F5, 281-A F4): привязка квитанций и notify к ТЕКУЩЕМУ шагу.
// Bluedroid выполняет GATT-операции соединения по очереди (FIFO), квитанция не несёт контекста.
// outstanding = отправлено и не подтверждено; квитанция, обнулившая счётчик, — последней записи,
// остальные — прежних (просроченных) и шагу не засчитываются. Сеанс стартует при outstanding == 0.
// Чистый модуль, хост-тест test/host/test_journal_track.c.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "radex_journal_parse.h"

/**
 * Структура отслеживания шага журнала.
 * Содержит информацию о текущем шаге сеанса: отправленные записи, ожидаемые уведомления,
 * подтверждения и статусы.
 */
typedef struct {
    uint8_t  outstanding;    /* записей отправлено и ещё не подтверждено (порядок FIFO Bluedroid) */
    bool     sent;           /* запись текущего шага отправлена */
    uint16_t handle;         /* handle записи текущего шага */
    bool     expect_notify;  /* шаг ждёт notify */
    bool     acked;          /* квитанция текущего шага получена */
    int      ack_status;     /* её статус ATT (0 = OK) */
    bool     notified;       /* notify текущего шага получен */
    uint16_t stale_acks;     /* квитанции прежних записей, поглощённые без зачёта */
    uint16_t unexpected;     /* события вне текущего шага: сеанс не может быть "ok" */
} radex_j_track_t;

/**
 * Тип события при обработке уведомлений и подтверждений.
 */
typedef enum {
    RJ_EV_FOREIGN    = 0,    /* не наш handle — не трогаем */
    RJ_EV_CURRENT    = 1,    /* засчитано текущему шагу */
    RJ_EV_STALE      = 2,    /* квитанция прежней записи, отброшена */
    RJ_EV_UNEXPECTED = 3     /* событие без шага, которому оно положено, отброшено */
} radex_j_ev_t;

/**
 * Сброс всех данных отслеживания.
 * Используется при разрыве соединения — очередь Bluedroid сброшена.
 */
void radex_j_track_reset_all(radex_j_track_t *t);

/**
 * Начало нового сеанса.
 * Сохраняет количество неподтверждённых записей, сбрасывает остальные поля.
 */
void radex_j_track_new_session(radex_j_track_t *t);

/**
 * Отправка записи текущего шага.
 * Увеличивает счётчик outstanding, устанавливает параметры шага.
 */
void radex_j_track_on_send(radex_j_track_t *t, uint16_t handle, bool expect_notify);

/**
 * Перевод шага в состояние ожидания между шагами.
 * Сбрасывает флаги отправки и подтверждения.
 */
void radex_j_track_step_idle(radex_j_track_t *t);

/**
 * Обработка подтверждения записи.
 * Возвращает тип события: RJ_EV_CURRENT, RJ_EV_STALE или RJ_EV_UNEXPECTED.
 */
radex_j_ev_t radex_j_track_on_ack(radex_j_track_t *t, uint16_t handle, int status);

/**
 * Обработка уведомления от устройства.
 * Возвращает RJ_EV_CURRENT если уведомление корректно, иначе RJ_EV_UNEXPECTED.
 */
radex_j_ev_t radex_j_track_on_notify(radex_j_track_t *t);

/**
 * Проверяет завершён ли текущий шаг.
 * Шаг завершён, если отправлен, подтверждён и получен notify (если ожидается).
 */
bool radex_j_track_step_done(const radex_j_track_t *t);

/**
 * Проверяет можно ли начать новый сеанс.
 * Новый сеанс можно начать только если нет неподтверждённых записей.
 */
bool radex_j_track_can_start(const radex_j_track_t *t);

/**
 * Определение финального статуса сеанса.
 * Заполняет строку статуса и возвращает true, если всё прошло успешно.
 */
bool radex_j_final_status(const radex_journal_t *j, const radex_j_track_t *t, char *st, size_t st_len);
