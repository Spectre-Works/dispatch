// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "notifications.h"

#include "esp_timer.h"
#include "local_tcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool notification_path_valid(const char *path)
{
    if (path[0] != '/' || path[1] == '/') return false;
    for (const char *c = path; *c; ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || strchr("/._~-", *c)) continue;
        return false;
    }
    return true;
}

bool notification_enqueue(notification_queue *queue, notification_kind kind, unsigned slot, uint32_t now)
{
    notification_job *available = NULL;
    for (unsigned i = 0; i < NOTIFICATION_QUEUE_SIZE; ++i) {
        notification_job *job = &queue->jobs[i];
        if (job->sequence && job->kind == kind && job->slot == slot) return kind != NOTICE_TEST;
        if (!job->sequence && !available) available = job;
    }
    if (!available) {
        if (kind != NOTICE_TEST) ++queue->dropped;
        return false;
    }
    if (++queue->sequence == 0) ++queue->sequence;
    *available = (notification_job){.sequence = queue->sequence, .due = now, .slot = slot, .kind = kind};
    if (kind == NOTICE_TEST) queue->test = NOTICE_PENDING;
    return true;
}

notification_job notification_due(const notification_queue *queue, uint32_t now)
{
    notification_job selected = {0};
    for (unsigned i = 0; i < NOTIFICATION_QUEUE_SIZE; ++i) {
        const notification_job *job = &queue->jobs[i];
        if (job->sequence && (int32_t)(now - job->due) >= 0 &&
            (!selected.sequence || (int32_t)(job->due - selected.due) < 0 ||
             (job->due == selected.due && (int32_t)(job->sequence - selected.sequence) < 0))) selected = *job;
    }
    return selected;
}

void notification_finish(notification_queue *queue, uint32_t sequence, notification_result result, uint32_t now)
{
    for (unsigned i = 0; i < NOTIFICATION_QUEUE_SIZE; ++i) {
        notification_job *job = &queue->jobs[i];
        if (job->sequence != sequence) continue;
        ++job->attempts;
        if (result == NOTICE_RETRY && job->attempts < 3) {
            job->due = now + job->attempts * 30;
        } else {
            if (result == NOTICE_ACCEPTED) ++queue->sent;
            else ++queue->failed;
            if (job->kind == NOTICE_TEST) queue->test = result == NOTICE_ACCEPTED ? NOTICE_DELIVERED : NOTICE_FAILED;
            *job = (notification_job){0};
        }
        return;
    }
}

static notification_result response_status(int fd, int64_t deadline)
{
    char response[13] = {0};
    size_t used = 0;
    while (used < sizeof(response)) {
        const int count = local_tcp_read(fd, response + used, sizeof(response) - used, deadline);
        if (count <= 0) return NOTICE_RETRY;
        used += count;
    }
    if ((memcmp(response, "HTTP/1.1 ", 9) && memcmp(response, "HTTP/1.0 ", 9)) ||
        response[12] != ' ' || response[9] < '1' || response[9] > '5' ||
        response[10] < '0' || response[10] > '9' || response[11] < '0' || response[11] > '9') return NOTICE_REJECTED;
    const int code = (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
    if (code >= 200 && code < 300) return NOTICE_ACCEPTED;
    return code == 408 || code == 429 || code >= 500 ? NOTICE_RETRY : NOTICE_REJECTED;
}

notification_result notification_post(const notification_target *target, const notification_job *job, uint32_t boot)
{
    static const char *const event_names[] = {
        [NOTICE_WORK_MISSING] = "work_missing", [NOTICE_SWITCH_HELD] = "switch_unconfirmed",
        [NOTICE_DESTINATION_CHANGED] = "destination_mismatch", [NOTICE_RECOVERY_EXHAUSTED] = "recovery_exhausted",
        [NOTICE_TEST] = "test",
    };
    if (!target->port || !notification_path_valid(target->path) ||
        job->kind >= sizeof(event_names) / sizeof(*event_names)) return NOTICE_REJECTED;
    char id[18];
    snprintf(id, sizeof(id), "%08lx-%08lx", (unsigned long)boot, (unsigned long)job->sequence);
    char payload[120], slot[4] = {0};
    if (job->kind != NOTICE_TEST) snprintf(slot, sizeof(slot), "%u", job->slot + 1u);
    const int bytes = snprintf(payload, sizeof(payload), "{\"version\":1,\"event\":\"%s\",\"id\":\"%s\",\"slot\":%s}",
                               event_names[job->kind], id, job->kind == NOTICE_TEST ? "null" : slot);
    char header[360];
    const int length = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nIdempotency-Key: %s\r\n\r\n",
        target->path, target->ip, target->port, bytes, id);
    if (bytes < 0 || (size_t)bytes >= sizeof(payload) || length < 0 || (size_t)length >= sizeof(header)) {
        return NOTICE_REJECTED;
    }
    const int64_t deadline = esp_timer_get_time() + 350000;
    const int fd = local_tcp_open(target->ip, target->port, deadline);
    if (fd < 0) return NOTICE_RETRY;
    notification_result result = NOTICE_RETRY;
    if (local_tcp_send(fd, header, length, deadline) && local_tcp_send(fd, payload, bytes, deadline)) {
        result = response_status(fd, deadline);
    }
    close(fd);
    return result;
}
