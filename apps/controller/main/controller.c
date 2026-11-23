// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "installation.h"
#include "network.h"
#include "profile_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

void web_start(void);

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    profile_store_start();
    network_start();
    installation_start();
    web_start();
    for (;;) {
        network_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
