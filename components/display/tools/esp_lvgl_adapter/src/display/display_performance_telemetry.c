/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display_performance_telemetry.h"

#include <stdbool.h>
#include <string.h>

#include "esp_lv_adapter_display.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
static portMUX_TYPE s_telemetry_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_lv_adapter_display_telemetry_t s_telemetry;

void display_performance_telemetry_record_software_image(uint64_t pixels, uint64_t elapsed_us) {
    portENTER_CRITICAL(&s_telemetry_lock);
    s_telemetry.software_image_calls++;
    s_telemetry.software_image_pixels += pixels;
    s_telemetry.software_image_elapsed_us += elapsed_us;
    portEXIT_CRITICAL(&s_telemetry_lock);
}

void esp_lv_adapter_display_telemetry_record_framebuffer_dma2d(uint64_t pixels, uint64_t elapsed_us) {
    portENTER_CRITICAL(&s_telemetry_lock);
    s_telemetry.framebuffer_dma2d_calls++;
    s_telemetry.framebuffer_dma2d_pixels += pixels;
    s_telemetry.framebuffer_dma2d_elapsed_us += elapsed_us;
    portEXIT_CRITICAL(&s_telemetry_lock);
}

void display_performance_telemetry_record_panel_submit(uint64_t elapsed_us) {
    portENTER_CRITICAL(&s_telemetry_lock);
    s_telemetry.panel_submit_calls++;
    s_telemetry.panel_submit_elapsed_us += elapsed_us;
    portEXIT_CRITICAL(&s_telemetry_lock);
}

void display_performance_telemetry_record_panel_vsync_wait(uint64_t elapsed_us) {
    portENTER_CRITICAL(&s_telemetry_lock);
    s_telemetry.panel_vsync_wait_calls++;
    s_telemetry.panel_vsync_wait_elapsed_us += elapsed_us;
    portEXIT_CRITICAL(&s_telemetry_lock);
}
#else
void display_performance_telemetry_record_software_image(uint64_t pixels, uint64_t elapsed_us) {
    (void)pixels;
    (void)elapsed_us;
}

void esp_lv_adapter_display_telemetry_record_framebuffer_dma2d(uint64_t pixels, uint64_t elapsed_us) {
    (void)pixels;
    (void)elapsed_us;
}

void display_performance_telemetry_record_panel_submit(uint64_t elapsed_us) { (void)elapsed_us; }

void display_performance_telemetry_record_panel_vsync_wait(uint64_t elapsed_us) { (void)elapsed_us; }
#endif

bool esp_lv_adapter_display_telemetry_take(esp_lv_adapter_display_telemetry_t* telemetry) {
    if (telemetry == NULL) {
        return false;
    }
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
    portENTER_CRITICAL(&s_telemetry_lock);
    *telemetry = s_telemetry;
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    portEXIT_CRITICAL(&s_telemetry_lock);
    return true;
#else
    memset(telemetry, 0, sizeof(*telemetry));
    return false;
#endif
}
