// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <openssl/rand.h>
#include <time.h>

const char *esp_err_to_name(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND) return "missing setting";
    if (error == ESP_ERR_INVALID_SIZE) return "invalid size";
    return error == ESP_OK ? "ok" : "platform error";
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

uint32_t esp_random(void)
{
    uint32_t number;
    if (RAND_bytes((unsigned char *)&number, sizeof(number)) != 1) abort();
    return number;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    return &mutex;
}

void xSemaphoreTake(SemaphoreHandle_t mutex, int wait)
{
    if (pthread_mutex_lock(mutex)) abort();
}

void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (pthread_mutex_unlock(mutex)) abort();
}

static void (*task)(void *);
static void *run_worker(void *arg)
{
    task(arg);
    return NULL;
}

int xTaskCreate(void (*worker)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    // Only the shared installation worker runs outside the serial HTTP loop.
    pthread_t thread;
    task = worker;
    return pthread_create(&thread, NULL, run_worker, arg) ? 0 : pdPASS;
}

void vTaskDelay(unsigned ms)
{
    struct timespec remaining = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) && errno == EINTR) {}
}
