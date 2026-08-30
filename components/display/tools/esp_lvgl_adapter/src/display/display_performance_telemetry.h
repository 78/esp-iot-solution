/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

void display_performance_telemetry_record_software_image(uint64_t pixels, uint64_t elapsed_us);
void display_performance_telemetry_record_panel_submit(uint64_t elapsed_us);
void display_performance_telemetry_record_panel_vsync_wait(uint64_t elapsed_us);
