// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "notifications.h"
#include "local_tcp.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *reply;
static size_t consumed, written;
static char request[512];

int64_t esp_timer_get_time(void)
{
    return 1000000;
}

int local_tcp_open(const char *ip, unsigned port, int64_t deadline)
{
    assert(strcmp(ip, "192.168.77.40") == 0 && port == 8080 && deadline == 1350000);
    return open("/dev/null", O_RDONLY);
}

bool local_tcp_send(int fd, const char *bytes, size_t size, int64_t deadline)
{
    assert(fd >= 0 && deadline == 1350000 && written + size < sizeof(request));
    memcpy(request + written, bytes, size);
    written += size;
    request[written] = 0;
    return true;
}

int local_tcp_read(int fd, char *bytes, size_t capacity, int64_t deadline)
{
    assert(fd >= 0 && capacity > 0 && deadline == 1350000);
    if (!reply[consumed]) return 0;
    *bytes = reply[consumed++];
    return 1;
}

static void queue(void)
{
    notification_queue q = {0};
    assert(notification_enqueue(&q, NOTICE_WORK_MISSING, 0, 100));
    assert(notification_enqueue(&q, NOTICE_WORK_MISSING, 0, 100));
    assert(notification_enqueue(&q, NOTICE_TEST, 0, 100));
    assert(!notification_enqueue(&q, NOTICE_TEST, 0, 100));
    assert(q.sequence == 2 && q.test == NOTICE_PENDING);
    notification_job work = notification_due(&q, 100);
    assert(work.kind == NOTICE_WORK_MISSING);
    notification_finish(&q, work.sequence, NOTICE_RETRY, 100);
    notification_job test = notification_due(&q, 101);
    assert(test.kind == NOTICE_TEST && test.sequence != work.sequence);
    notification_finish(&q, test.sequence, NOTICE_ACCEPTED, 101);
    assert(q.sent == 1 && q.test == NOTICE_DELIVERED);
    assert(!notification_due(&q, 129).sequence);
    assert(notification_due(&q, 130).sequence == work.sequence);
    notification_finish(&q, work.sequence, NOTICE_RETRY, 130);
    assert(!notification_due(&q, 189).sequence);
    assert(notification_due(&q, 190).sequence == work.sequence);
    notification_finish(&q, work.sequence, NOTICE_RETRY, 190);
    assert(q.failed == 1 && !notification_due(&q, 250).sequence);
    for (unsigned i = 0; i < NOTIFICATION_QUEUE_SIZE; ++i) {
        assert(notification_enqueue(&q, NOTICE_SWITCH_HELD, i, 200));
    }
    assert(!notification_enqueue(&q, NOTICE_DESTINATION_CHANGED, 0, 200) && q.dropped == 1);
}

static void post(void)
{
    notification_target target = {.ip = "192.168.77.40", .port = 8080, .path = "/api/webhook/synthetic"};
    notification_job job = {.sequence = 17, .kind = NOTICE_SWITCH_HELD, .slot = 1};
    assert(notification_path_valid(target.path));
    assert(!notification_path_valid("//different-host/path"));
    assert(!notification_path_valid("/path\r\nGET /"));
    assert(!notification_path_valid("/query?key=value"));
    reply = "HTTP/1.1 204 No Content\r\n";
    assert(notification_post(&target, &job, 5) == NOTICE_ACCEPTED);
    assert(strstr(request, "POST /api/webhook/synthetic HTTP/1.1\r\n"));
    assert(strstr(request, "Idempotency-Key: 00000005-00000011\r\n"));
    assert(strstr(request, "\"event\":\"switch_unconfirmed\",\"id\":\"00000005-00000011\",\"slot\":2}"));
    const char *body = strstr(request, "\r\n\r\n");
    const char *length = strstr(request, "Content-Length: ");
    assert(body && length && strlen(body + 4) == (size_t)atoi(length + 16));

    consumed = written = 0;
    reply = "HTTP/1.1 429 Too Many Requests\r\n";
    assert(notification_post(&target, &job, 5) == NOTICE_RETRY);
    consumed = written = 0;
    reply = "HTTP/1.1 302 Moved Temporarily\r\n";
    assert(notification_post(&target, &job, 5) == NOTICE_REJECTED);
    consumed = written = 0;
    reply = "HTTP";
    assert(notification_post(&target, &job, 5) == NOTICE_RETRY);
    consumed = written = 0;
    reply = "HTTP/1.0 200 OK\r\n";
    job.kind = NOTICE_TEST;
    assert(notification_post(&target, &job, 5) == NOTICE_ACCEPTED);
    assert(strstr(request, "\"event\":\"test\",\"id\":\"00000005-00000011\",\"slot\":null}"));
}

int main(void)
{
    queue();
    post();
    puts("DISPATCH notification queue and HTTP tests passed");
}
