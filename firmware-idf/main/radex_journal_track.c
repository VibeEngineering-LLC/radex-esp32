#include "radex_journal_track.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Сбросить все счетчики и флаги (разрыв соединения: очередь Bluedroid сброшена)
 */
void radex_j_track_reset_all(radex_j_track_t *t) {
    if (t == NULL) return;
    memset(t, 0, sizeof(*t));
}

/**
 * Начать новый сеанс (сохранить счетчик outstanding, остальное обнулить)
 */
void radex_j_track_new_session(radex_j_track_t *t) {
    if (t == NULL) return;
    uint8_t out = t->outstanding;
    memset(t, 0, sizeof(*t));
    t->outstanding = out;
}

/**
 * Отправка записи текущего шага
 */
void radex_j_track_on_send(radex_j_track_t *t, uint16_t handle, bool expect_notify) {
    if (t == NULL) return;
    if (t->outstanding < 255) t->outstanding++;
    t->sent = true;
    t->handle = handle;
    t->expect_notify = expect_notify;
    t->acked = false;
    t->ack_status = 0;
    t->notified = false;
}

/**
 * Шаг ожидает завершения (между шагами / во время задержки)
 */
void radex_j_track_step_idle(radex_j_track_t *t) {
    if (t == NULL) return;
    t->sent = false;
    t->acked = false;
    t->ack_status = 0;
    t->notified = false;
    t->expect_notify = false;
}

/**
 * Обработка подтверждения записи
 */
radex_j_ev_t radex_j_track_on_ack(radex_j_track_t *t, uint16_t handle, int status) {
    if (t == NULL) return RJ_EV_FOREIGN;
    if (handle != RADEX_NUS_H_RX && handle != RADEX_NUS_H_CCCD) return RJ_EV_FOREIGN;
    if (t->outstanding == 0) {
        t->unexpected++;
        return RJ_EV_UNEXPECTED;
    }
    t->outstanding--;
    if (t->outstanding > 0) {
        t->stale_acks++;
        return RJ_EV_STALE;
    }
    if (!t->sent) {
        t->stale_acks++;
        return RJ_EV_STALE;
    }
    if (t->acked || handle != t->handle) {
        t->unexpected++;
        return RJ_EV_UNEXPECTED;
    }
    t->acked = true;
    t->ack_status = status;
    return RJ_EV_CURRENT;
}

/**
 * Обработка уведомления
 */
radex_j_ev_t radex_j_track_on_notify(radex_j_track_t *t) {
    if (t == NULL) return RJ_EV_FOREIGN;
    if (t->sent && t->expect_notify && !t->notified) {
        t->notified = true;
        return RJ_EV_CURRENT;
    }
    t->unexpected++;
    return RJ_EV_UNEXPECTED;
}

/**
 * Проверить завершение текущего шага
 */
bool radex_j_track_step_done(const radex_j_track_t *t) {
    if (t == NULL) return false;
    return t->sent && t->acked && t->ack_status == 0 && (!t->expect_notify || t->notified);
}

/**
 * Можно ли начать новый шаг
 */
bool radex_j_track_can_start(const radex_j_track_t *t) {
    if (t == NULL) return false;
    return t->outstanding == 0;
}

/**
 * Определить финальный статус сеанса
 */
bool radex_j_final_status(const radex_journal_t *j, const radex_j_track_t *t, char *st, size_t st_len) {
    if (j == NULL || t == NULL || st == NULL || st_len == 0) return false;

    unsigned shorts = 0;
    for (int i = 0; i < (int)j->n_summary_pkt && i < RADEX_J_MAX_PKT; i++) {
        radex_journal_summary_t s;
        if (radex_journal_parse_summary(j->summary_pkt[i].data, j->summary_pkt[i].len, &s) == RADEX_J_SHORT)
            shorts++;
    }
    for (int i = 0; i < (int)j->n_record_pkt && i < RADEX_J_MAX_REC + 1; i++) {
        radex_journal_record_t r;
        if (radex_journal_parse_record(j->record_pkt[i].data, j->record_pkt[i].len, &r) == RADEX_J_SHORT)
            shorts++;
    }

    if (t->unexpected > 0) {
        snprintf(st, st_len, "unexpected events: %u", (unsigned)t->unexpected);
        return false;
    }
    if (shorts > 0) {
        snprintf(st, st_len, "short packet: %u (MTU?)", (unsigned)shorts);
        return false;
    }
    if (j->n_records == 0) {
        snprintf(st, st_len, "no records");
        return false;
    }

    /* журнал длиннее RADEX_J_MAX_REC: записи верны, но не все */
    snprintf(st, st_len, "%s", j->truncated ? "truncated" : "ok");
    return true;
}
