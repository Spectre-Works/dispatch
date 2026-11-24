// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "installation.h"
#include "network.h"
#include "network_update.h"
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
    network_update_start();
    installation_start();
    web_start();
    bool checked_boot = false;
    for (;;) {
        network_tick();
        if (!checked_boot) {
            const bool healthy = installation_boot_healthy();
            if (healthy || network_uptime() >= 15) {
                network_update_confirm_boot(healthy);
                checked_boot = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
