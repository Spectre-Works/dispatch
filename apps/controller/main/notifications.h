// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_NOTIFICATIONS_H
#define DISPATCH_NOTIFICATIONS_H

#include <stdbool.h>
#include <stdint.h>

enum { NOTIFICATION_PATH_BYTES = 128, NOTIFICATION_QUEUE_SIZE = 8 };
typedef struct {
    char ip[16];
    char path[NOTIFICATION_PATH_BYTES];
    uint16_t port;
} notification_target;

typedef enum {
    NOTICE_WORK_MISSING, NOTICE_SWITCH_HELD, NOTICE_DESTINATION_CHANGED, NOTICE_RECOVERY_EXHAUSTED, NOTICE_TEST,
} notification_kind;

typedef enum { NOTICE_IDLE, NOTICE_PENDING, NOTICE_DELIVERED, NOTICE_FAILED } notification_state;
typedef enum { NOTICE_ACCEPTED, NOTICE_RETRY, NOTICE_REJECTED } notification_result;

typedef struct {
    uint32_t sequence, due;
    uint8_t slot, kind, attempts;
} notification_job;

typedef struct {
    notification_job jobs[NOTIFICATION_QUEUE_SIZE];
    uint32_t sequence;
    uint32_t sent, failed, dropped;
    notification_state test;
} notification_queue;

bool notification_path_valid(const char *path);
bool notification_enqueue(notification_queue *queue, notification_kind kind, unsigned slot, uint32_t now);
notification_job notification_due(const notification_queue *queue, uint32_t now);
void notification_finish(notification_queue *queue, uint32_t sequence, notification_result result, uint32_t now);
notification_result notification_post(const notification_target *target, const notification_job *job, uint32_t boot);
#endif
