/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display Bridge Common - Shared implementation for display bridge operations
 */

/*********************
 *      INCLUDES
 *********************/
#include "display_bridge_common.h"

#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 1)
#include "esp_efuse.h"
#else
#include "esp_flash_encrypt.h"
#endif
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "esp_lvgl:bridge";

/**********************
 *  FLASH ENC DMA ALIGN
 **********************/

#if defined(SOC_GDMA_EXT_MEM_ENC_ALIGNMENT)
#define DISPLAY_BRIDGE_ENC_DMA_ALIGN  SOC_GDMA_EXT_MEM_ENC_ALIGNMENT
#else
#define DISPLAY_BRIDGE_ENC_DMA_ALIGN  16
#endif

bool display_bridge_flash_encryption_active(void)
{
    static int s_state = -1;
    if (s_state < 0) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 1)
        s_state = esp_efuse_is_flash_encryption_enabled() ? 1 : 0;
#else
        s_state = esp_flash_encryption_enabled() ? 1 : 0;
#endif
    }
    return s_state == 1;
}

size_t display_bridge_dma2d_ext_mem_alignment(void)
{
    return DISPLAY_BRIDGE_ENC_DMA_ALIGN;
}

static bool display_bridge_psram_is_no_enc(const void *buffer)
{
#if CONFIG_SPIRAM_ENC_EXEMPT
    return buffer && esp_psram_ptr_is_no_enc(buffer);
#else
    (void)buffer;
    return false;
#endif
}

static size_t display_bridge_gcd_size(size_t a, size_t b)
{
    while (b != 0) {
        size_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static size_t display_bridge_dma2d_align_pixels(uint8_t color_bytes)
{
    size_t align = display_bridge_dma2d_ext_mem_alignment();
    size_t gcd = display_bridge_gcd_size(align, color_bytes);
    return align / gcd;
}

bool display_bridge_dma2d_buffer_needs_alignment(const void *buffer)
{
    if (!buffer || !display_bridge_flash_encryption_active()) {
        return false;
    }

    if (!esp_ptr_external_ram(buffer)) {
        return false;
    }

    return !display_bridge_psram_is_no_enc(buffer);
}

bool display_bridge_dma2d_x_rounding_sufficient(const void *buffer,
                                                size_t stride_px,
                                                uint8_t color_bytes)
{
    if (!display_bridge_dma2d_buffer_needs_alignment(buffer) || color_bytes == 0) {
        return false;
    }

    size_t stride_bytes = stride_px * color_bytes;
    return (stride_bytes & (display_bridge_dma2d_ext_mem_alignment() - 1)) == 0;
}

bool display_bridge_dma2d_window_is_compatible(const void *buffer,
                                               size_t stride_px,
                                               size_t offset_x_px,
                                               size_t copy_width_px,
                                               uint8_t color_bytes)
{
    if (!buffer || color_bytes == 0) {
        return false;
    }

    if (!display_bridge_dma2d_buffer_needs_alignment(buffer)) {
        return true;
    }

    const size_t align = display_bridge_dma2d_ext_mem_alignment();
    const uintptr_t base = (uintptr_t)buffer;
    const size_t stride_bytes = stride_px * color_bytes;
    const size_t offset_bytes = offset_x_px * color_bytes;
    const size_t width_bytes = copy_width_px * color_bytes;

    return ((base & (align - 1)) == 0) &&
           ((stride_bytes & (align - 1)) == 0) &&
           ((offset_bytes & (align - 1)) == 0) &&
           ((width_bytes & (align - 1)) == 0);
}

void display_bridge_align_area_for_enc_dma(lv_area_t *area, int hor_res, uint8_t color_bytes)
{
    if (!area || color_bytes == 0) {
        return;
    }

    const int align_px = (int)display_bridge_dma2d_align_pixels(color_bytes);
    int x1 = ((int)area->x1 / align_px) * align_px;
    int x2 = ((((int)area->x2 + 1) + align_px - 1) / align_px) * align_px - 1;

    if (x1 < 0) {
        x1 = 0;
    }
    if (hor_res > 0 && x2 > hor_res - 1) {
        x2 = hor_res - 1;
    }

    area->x1 = x1;
    area->x2 = x2;
}

/**********************
 *   CACHE PROFILE
 **********************/

static size_t s_bridge_cache_line_size = 0;
static int s_bridge_block_size_small = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_SMALL_DEFAULT;
static int s_bridge_block_size_large = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_LARGE_DEFAULT;

static void display_bridge_init_cache_profile(void)
{
    if (s_bridge_cache_line_size) {
        return;
    }

    size_t align = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_INTERNAL, &align) != ESP_OK || align == 0) {
        if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) != ESP_OK || align == 0) {
            align = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_SMALL_DEFAULT;
        }
    }

    if (align < ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_SMALL_DEFAULT) {
        align = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_SMALL_DEFAULT;
    }

    s_bridge_cache_line_size = align;

    int block_small = (int)align;
    if (block_small <= 0) {
        block_small = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_SMALL_DEFAULT;
    }

    int block_large = block_small * 8;
    if (block_large < ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_LARGE_DEFAULT) {
        block_large = ESP_LV_ADAPTER_BRIDGE_BLOCK_SIZE_LARGE_DEFAULT;
    }
    if (block_large > 512) {
        block_large = 512;
    }

    s_bridge_block_size_small = block_small;
    s_bridge_block_size_large = block_large;
}

/**********************
 *   DIRTY REGION MANAGEMENT
 **********************/

/**
 * @brief Capture current dirty region from LVGL
 */
void display_dirty_region_capture(esp_lv_adapter_display_dirty_region_t *dst,
                                  const lv_area_t *areas,
                                  const uint8_t *joined,
                                  uint16_t count)
{
    if (!dst) {
        return;
    }

    uint16_t stored = LV_MIN(count, (uint16_t)LV_INV_BUF_SIZE);

    if (!areas || !joined || stored == 0) {
        dst->inv_p = 0;
        memset(dst->inv_area_joined, 0, sizeof(dst->inv_area_joined));
        return;
    }

    dst->inv_p = stored;
    for (uint16_t i = 0; i < stored; i++) {
        dst->inv_area_joined[i] = joined[i];
        dst->inv_areas[i] = areas[i];
    }
}

/**********************
 *   ROTATION & COPY OPERATIONS
 **********************/

/**
 * @brief Rotate and copy a region with stride support (IRAM optimized)
 *
 * Uses block-based copying for better cache locality
 */
void IRAM_ATTR display_rotate_copy_region(const void *src,
                                          void *dst_fb,
                                          uint16_t lv_x_start,
                                          uint16_t lv_y_start,
                                          uint16_t lv_x_end,
                                          uint16_t lv_y_end,
                                          uint16_t src_stride_px,
                                          uint16_t hor_res,
                                          uint16_t ver_res,
                                          esp_lv_adapter_rotation_t rotation,
                                          uint8_t color_bytes,
                                          int block_size_small,
                                          int block_size_large)
{
    const int rect_w = lv_x_end - lv_x_start + 1;
    const int rect_h = lv_y_end - lv_y_start + 1;
    const int phys_w = hor_res;

    if (rotation == ESP_LV_ADAPTER_ROTATE_0) {
        const uint8_t *src_base = (const uint8_t *)src;
        uint8_t *dst_base = (uint8_t *)dst_fb + (size_t)(lv_y_start * phys_w + lv_x_start) * color_bytes;
        size_t row_bytes = rect_w * color_bytes;
        size_t dst_stride_bytes = phys_w * color_bytes;
        size_t src_stride_bytes = src_stride_px * color_bytes;

        bool is_contiguous = (src_stride_px == rect_w) && (rect_w == phys_w) && (lv_x_start == 0);

        if (is_contiguous) {
            size_t total_bytes = rect_h * row_bytes;
            memcpy(dst_base, src_base, total_bytes);
        } else {
            for (int y = 0; y < rect_h; y++) {
                memcpy(dst_base, src_base, row_bytes);
                src_base += src_stride_bytes;
                dst_base += dst_stride_bytes;
            }
        }
        return;
    }

    if (rotation == ESP_LV_ADAPTER_ROTATE_180) {
        for (int y = 0; y < rect_h; y++) {
            const uint8_t *src_row = (const uint8_t *)src + (size_t)y * src_stride_px * color_bytes;
            int dst_y = ver_res - 1 - (lv_y_start + y);
            int dst_x_start = hor_res - 1 - lv_x_end;
            uint8_t *dst_row = (uint8_t *)dst_fb + (size_t)(dst_y * phys_w + dst_x_start) * color_bytes;

            if (color_bytes == 2) {
                uint16_t *dst16 = (uint16_t *)(dst_row + (rect_w - 1) * 2);
                const uint16_t *src16 = (const uint16_t *)src_row;
                for (int x = 0; x < rect_w; x++) {
                    *dst16-- = *src16++;
                }
            } else if (color_bytes == 4) {
                uint32_t *dst32 = (uint32_t *)(dst_row + (rect_w - 1) * 4);
                const uint32_t *src32 = (const uint32_t *)src_row;
                for (int x = 0; x < rect_w; x++) {
                    *dst32-- = *src32++;
                }
            } else {
                for (int x = 0; x < rect_w; x++) {
                    const uint8_t *src_pixel = src_row + (size_t)x * color_bytes;
                    uint8_t *dst_pixel = dst_row + (size_t)(rect_w - 1 - x) * color_bytes;
                    memcpy(dst_pixel, src_pixel, color_bytes);
                }
            }
        }
        return;
    }

    /* Select block size based on rotation for optimal cache usage */
    int block_w = (rotation == ESP_LV_ADAPTER_ROTATE_90 || rotation == ESP_LV_ADAPTER_ROTATE_270) ? block_size_small : block_size_large;
    int block_h = (rotation == ESP_LV_ADAPTER_ROTATE_90 || rotation == ESP_LV_ADAPTER_ROTATE_270) ? block_size_large : block_size_small;

    /* Process in blocks for better cache locality */
    for (int i = 0; i < rect_h; i += block_h) {
        int max_height = (i + block_h > rect_h) ? rect_h : i + block_h;

        for (int j = 0; j < rect_w; j += block_w) {
            int max_width = (j + block_w > rect_w) ? rect_w : j + block_w;

            /* Copy pixels within current block - fully inlined for performance */
            for (int y = i; y < max_height; y++) {
                for (int x = j; x < max_width; x++) {
                    int gx = lv_x_start + x;
                    int gy = lv_y_start + y;
                    int dx, dy;

                    /* Inline coordinate transformation - eliminates function call overhead */
                    if (rotation == ESP_LV_ADAPTER_ROTATE_90) {
                        dx = phys_w - 1 - gy;
                        dy = gx;
                    } else { /* ESP_LV_ADAPTER_ROTATE_270 */
                        dx = gy;
                        dy = ver_res - 1 - gx;
                    }

                    const uint8_t *src_pixel = (const uint8_t *)src + (size_t)(y * src_stride_px + x) * color_bytes;
                    uint8_t *dst_pixel = (uint8_t *)dst_fb + (size_t)(dy * phys_w + dx) * color_bytes;

                    /* Inline pixel copy - eliminates function call overhead */
                    if (color_bytes == 2) {
                        *(uint16_t *)dst_pixel = *(const uint16_t *)src_pixel;
                    } else if (color_bytes == 3) {
                        dst_pixel[0] = src_pixel[0];
                        dst_pixel[1] = src_pixel[1];
                        dst_pixel[2] = src_pixel[2];
                    }
                }
            }
        }
    }
}

/**
 * @brief Rotate an entire image (IRAM optimized)
 *
 * Uses block-based rotation for better cache locality
 */
void IRAM_ATTR display_rotate_image(const void *src,
                                    void *dst,
                                    int width,
                                    int height,
                                    int rotation,
                                    uint8_t color_bytes,
                                    int block_size_small,
                                    int block_size_large)
{
    /* Select block size based on rotation */
    int block_w = (rotation == 90 || rotation == 270) ? block_size_small : block_size_large;
    int block_h = (rotation == 90 || rotation == 270) ? block_size_large : block_size_small;

    /* Process in blocks */
    for (int i = 0; i < height; i += block_h) {
        int max_height = (i + block_h > height) ? height : i + block_h;

        for (int j = 0; j < width; j += block_w) {
            int max_width = (j + block_w > width) ? width : j + block_w;

            /* Rotate pixels within current block - fully inlined for performance */
            for (int x = i; x < max_height; x++) {
                for (int y = j; y < max_width; y++) {
                    const uint8_t *src_pixel = (const uint8_t *)src + (size_t)(x * width + y) * color_bytes;
                    uint8_t *dst_pixel;

                    switch (rotation) {
                    case 270:
                        dst_pixel = (uint8_t *)dst + (size_t)((width - 1 - y) * height + x) * color_bytes;
                        break;
                    case 180:
                        dst_pixel = (uint8_t *)dst + (size_t)((height - 1 - x) * width + (width - 1 - y)) * color_bytes;
                        break;
                    case 90:
                        dst_pixel = (uint8_t *)dst + (size_t)(y * height + (height - 1 - x)) * color_bytes;
                        break;
                    default:
                        return;
                    }

                    /* Inline pixel copy - eliminates function call overhead */
                    if (color_bytes == 2) {
                        *(uint16_t *)dst_pixel = *(const uint16_t *)src_pixel;
                    } else if (color_bytes == 3) {
                        dst_pixel[0] = src_pixel[0];
                        dst_pixel[1] = src_pixel[1];
                        dst_pixel[2] = src_pixel[2];
                    }
                }
            }
        }
    }
}

/**********************
 *   CACHE MANAGEMENT
 **********************/

size_t display_bridge_get_cache_line_size_by_addr(const void *addr)
{
    if (!addr) {
        return 0;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return esp_cache_get_line_size_by_addr(addr);
#else
    size_t align = 0;
    uint32_t caps = esp_ptr_external_ram(addr) ? MALLOC_CAP_SPIRAM :
                    esp_ptr_internal(addr)      ? MALLOC_CAP_INTERNAL : 0;
    if (caps == 0 || esp_cache_get_alignment(caps, &align) != ESP_OK) {
        return 0;
    }
    return align;
#endif
}

/**
 * @brief Synchronize cache for a specific memory range
 *
 * Note: Not placed in IRAM as it calls esp_cache_msync() which is also not in IRAM
 */
void display_cache_msync_range(const void *addr,
                               size_t size,
                               size_t cache_line_size)
{
    if (!addr || size == 0) {
        return;
    }

    size_t line_size = display_bridge_get_cache_line_size_by_addr(addr);
    if (line_size == 0) {
        return;
    }

    cache_line_size = line_size;

    /* Align to cache line boundaries */
    uintptr_t start_addr = (uintptr_t)addr;
    uintptr_t aligned_start = start_addr & ~((uintptr_t)cache_line_size - 1);
    size_t align_padding = start_addr - aligned_start;
    size_t aligned_size = ((size + align_padding + cache_line_size - 1) / cache_line_size) * cache_line_size;

    esp_cache_msync((void *)aligned_start, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/**
 * @brief Synchronize cache for a framebuffer (allows unaligned)
 *
 * Note: Not placed in IRAM as it calls esp_cache_msync() which is also not in IRAM
 */
void display_cache_msync_framebuffer(void *buffer,
                                     size_t size)
{
    if (!buffer || size == 0) {
        return;
    }

    if (display_bridge_get_cache_line_size_by_addr(buffer) == 0) {
        return;
    }

    esp_cache_msync(buffer, size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void display_cache_msync_invalidate_framebuffer(void *buffer,
                                                size_t size)
{
    if (!buffer || size == 0) {
        return;
    }

    size_t line_size = display_bridge_get_cache_line_size_by_addr(buffer);
    if (line_size == 0) {
        return;
    }

    uintptr_t start_addr = (uintptr_t)buffer;
    uintptr_t aligned_start = start_addr & ~((uintptr_t)line_size - 1);
    size_t align_padding = start_addr - aligned_start;
    size_t aligned_size = ((size + align_padding + line_size - 1) / line_size) * line_size;

    esp_cache_msync((void *)aligned_start, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

/**********************
 *   LCD OPERATIONS
 **********************/

/**
 * @brief Send full framebuffer to LCD panel
 */
esp_err_t display_lcd_blit_full(esp_lcd_panel_handle_t panel,
                                const esp_lv_adapter_display_runtime_info_t *runtime,
                                void *frame_buffer)
{
    if (!panel || !runtime || !frame_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_lcd_panel_draw_bitmap(panel,
                                     0,
                                     0,
                                     runtime->hor_res,
                                     runtime->ver_res,
                                     frame_buffer);
}

/**
 * @brief Send framebuffer region to LCD panel
 */
esp_err_t display_lcd_blit_area(esp_lcd_panel_handle_t panel,
                                int x_start,
                                int y_start,
                                int x_end,
                                int y_end,
                                const void *frame_buffer)
{
    if (!panel || !frame_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_lcd_panel_draw_bitmap(panel,
                                     x_start,
                                     y_start,
                                     x_end,
                                     y_end,
                                     frame_buffer);
}

/**
 * @brief Acquire next available frame buffer for double/triple buffering
 */
void *display_runtime_acquire_next_buffer(esp_lv_adapter_display_runtime_info_t *runtime,
                                          void **toggle_slot)
{
    if (!runtime || !toggle_slot) {
        return NULL;
    }

    void **frame_buffers = runtime->frame_buffers;
    uint8_t count = runtime->frame_buffer_count;

    /* No buffers available */
    if (count == 0 || frame_buffers[0] == NULL) {
        *toggle_slot = NULL;
        return NULL;
    }

    /* Single buffer mode */
    if (count == 1 || frame_buffers[1] == NULL) {
        *toggle_slot = frame_buffers[0];
        return frame_buffers[0];
    }

    /* Double buffering - toggle between two buffers */
    void *primary = frame_buffers[0];
    void *secondary = frame_buffers[1];
    void *current = *toggle_slot;

    if (current != primary && current != secondary) {
        current = secondary;
    } else {
        current = (current == primary) ? secondary : primary;
    }

    *toggle_slot = current;
    return current;
}

void display_bridge_get_block_sizes(int *block_small, int *block_large)
{
    display_bridge_init_cache_profile();
    if (block_small) {
        *block_small = s_bridge_block_size_small;
    }
    if (block_large) {
        *block_large = s_bridge_block_size_large;
    }
}

size_t display_bridge_get_cache_line_size(void)
{
    display_bridge_init_cache_profile();
    return s_bridge_cache_line_size;
}

/******************************************************************************
 *                   Bridge-Specific Common Functions (v8/v9)
 ******************************************************************************/

#include "esp_log.h"
#include "esp_check.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#endif

#if SOC_LCDCAM_RGB_LCD_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif

#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif

#if SOC_DMA2D_SUPPORTED
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
#include "esp_async_color_convert.h"
#else
#include "esp_async_fbcpy.h"
#endif

static esp_lv_adapter_display_bridge_hw_resource_t s_hw_resource = {0};

#if CONFIG_SOC_DMA2D_SUPPORTED && defined(ESP_ASYNC_COLOR_CONVERT_AVAILABLE)
/* Direct 2D-DMA copy path (see display_bridge_dma2d_copy_sync).
 *
 * esp_async_color_convert writes back the *whole* source picture and writes
 * back + invalidates the *whole* destination picture on every request. For a
 * 720x720 RGB888 frame that is ~3 MB of cache maintenance per copy (~1-2 ms)
 * and it evicts every other buffer from the shared L2 cache, which also slows
 * the composing core. Same-format copies therefore run on our own descriptors
 * and only synchronize the rows the block touches, exactly like the PPA driver
 * does for its extended windows. Format conversions keep using the helper. */
#include "esp_private/dma2d.h"
#include "hal/dma2d_ll.h"
#include "hal/dma2d_types.h"
#include "soc/dma2d_channel.h"

#define BRIDGE_DIRECT_COPY_BURST_LENGTH (128)

typedef struct {
    dma2d_pool_handle_t pool;
    dma2d_descriptor_t *tx_desc;
    dma2d_descriptor_t *rx_desc;
    size_t desc_size;
    dma2d_trans_t *trans_placeholder;
    dma2d_trans_config_t trans_config;
} bridge_direct_copy_t;

static bridge_direct_copy_t s_direct_copy;

static bool IRAM_ATTR s_direct_copy_on_rx_eof(dma2d_channel_handle_t chan, dma2d_event_data_t *event, void *user_data)
{
    (void)chan;
    (void)event;
    (void)user_data;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)s_hw_resource.dma2d_done_sem, &woken);
    return woken == pdTRUE;
}

static bool s_direct_copy_on_job_picked(uint32_t channel_count, const dma2d_trans_channel_info_t *channels,
                                        void *user_config)
{
    (void)user_config;
    dma2d_channel_handle_t tx_chan = NULL;
    dma2d_channel_handle_t rx_chan = NULL;
    for (uint32_t i = 0; i < channel_count; ++i) {
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_TX) {
            tx_chan = channels[i].chan;
        } else {
            rx_chan = channels[i].chan;
        }
    }

    dma2d_trigger_t trigger = {
        .periph = DMA2D_TRIG_PERIPH_M2M,
        .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX,
    };
    dma2d_connect(tx_chan, &trigger);
    trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    dma2d_connect(rx_chan, &trigger);

    dma2d_transfer_ability_t ability = {
        .desc_burst_en = true,
        .data_burst_length = BRIDGE_DIRECT_COPY_BURST_LENGTH,
        .access_ext_mem = true,
        .mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE,
    };
    dma2d_set_transfer_ability(tx_chan, &ability);
    dma2d_set_transfer_ability(rx_chan, &ability);

    dma2d_csc_config_t tx_csc = {
        .tx_csc_option = DMA2D_CSC_TX_NONE,
        .pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
        .post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
    };
    dma2d_csc_config_t rx_csc = {
        .rx_csc_option = DMA2D_CSC_RX_NONE,
        .pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
        .post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0,
    };
    dma2d_configure_color_space_conversion(tx_chan, &tx_csc);
    dma2d_configure_color_space_conversion(rx_chan, &rx_csc);

    dma2d_rx_event_callbacks_t callbacks = {
        .on_recv_eof = s_direct_copy_on_rx_eof,
    };
    dma2d_register_rx_event_callbacks(rx_chan, &callbacks, NULL);

    dma2d_set_desc_addr(rx_chan, (intptr_t)s_direct_copy.rx_desc);
    dma2d_set_desc_addr(tx_chan, (intptr_t)s_direct_copy.tx_desc);
    dma2d_start(rx_chan);
    dma2d_start(tx_chan);
    return false;
}

static void s_direct_copy_release(void)
{
    if (s_direct_copy.pool) {
        dma2d_release_pool(s_direct_copy.pool);
    }
    heap_caps_free(s_direct_copy.tx_desc);
    heap_caps_free(s_direct_copy.rx_desc);
    heap_caps_free(s_direct_copy.trans_placeholder);
    memset(&s_direct_copy, 0, sizeof(s_direct_copy));
}

/* Best effort: when anything fails the helper path stays in use. */
static void s_direct_copy_install(void)
{
    size_t alignment = 0;
    esp_cache_get_alignment(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, &alignment);
    size_t desc_size = alignment > sizeof(dma2d_descriptor_t) ? alignment : sizeof(dma2d_descriptor_t);
    if (desc_size % DMA2D_LL_DESC_ALIGNMENT) {
        desc_size += DMA2D_LL_DESC_ALIGNMENT - desc_size % DMA2D_LL_DESC_ALIGNMENT;
    }
    size_t desc_alignment = alignment > DMA2D_LL_DESC_ALIGNMENT ? alignment : DMA2D_LL_DESC_ALIGNMENT;
    s_direct_copy.desc_size = desc_size;
    s_direct_copy.tx_desc = heap_caps_aligned_calloc(desc_alignment, 1, desc_size,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_direct_copy.rx_desc = heap_caps_aligned_calloc(desc_alignment, 1, desc_size,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_direct_copy.trans_placeholder = heap_caps_calloc(1, dma2d_get_trans_elm_size(),
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_direct_copy.tx_desc || !s_direct_copy.rx_desc || !s_direct_copy.trans_placeholder) {
        ESP_LOGW(TAG, "Direct DMA2D copy unavailable (no descriptor memory); using helper path");
        s_direct_copy_release();
        return;
    }
    dma2d_pool_config_t pool_config = { .pool_id = 0 };
    if (dma2d_acquire_pool(&pool_config, &s_direct_copy.pool) != ESP_OK) {
        ESP_LOGW(TAG, "Direct DMA2D copy unavailable (no pool); using helper path");
        s_direct_copy.pool = NULL;
        s_direct_copy_release();
        return;
    }
    s_direct_copy.trans_config = (dma2d_trans_config_t) {
        .tx_channel_num = 1,
        .rx_channel_num = 1,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING,
        .on_job_picked = s_direct_copy_on_job_picked,
        .user_config = NULL,
    };
    ESP_LOGI(TAG, "Direct DMA2D copy with windowed cache sync enabled");
}

static void s_direct_copy_setup_desc(dma2d_descriptor_t *desc, const void *buffer, uint32_t pic_w, uint32_t pic_h,
                                     uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pbyte)
{
    memset(desc, 0, sizeof(*desc));
    desc->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->suc_eof = 1;
    desc->dma2d_en = 1;
    desc->ha_length = pic_w;
    desc->va_size = pic_h;
    desc->hb_length = w;
    desc->vb_size = h;
    desc->x = x;
    desc->y = y;
    desc->pbyte = pbyte;
    desc->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    desc->buffer = (void *)buffer;
    desc->next = NULL;
}

/* Writes back (and optionally invalidates) the rows a block touches: whole
 * rows between the first and last touched byte, one msync per block. */
static esp_err_t s_direct_copy_sync_rows(const void *base, size_t stride_bytes, uint32_t x, uint32_t y, uint32_t w,
                                         uint32_t h, uint32_t bytes_per_pixel, int flags)
{
    if (esp_cache_get_line_size_by_addr((void *)base) == 0) {
        return ESP_OK;
    }
    size_t begin = (size_t)y * stride_bytes + (size_t)x * bytes_per_pixel;
    size_t end = (size_t)(y + h - 1) * stride_bytes + (size_t)(x + w) * bytes_per_pixel;
    return esp_cache_msync((void *)((const uint8_t *)base + begin), end - begin,
                           flags | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* Returns ESP_ERR_NOT_SUPPORTED when the request must go through the helper. */
static esp_err_t s_direct_copy_run(const async_color_convert_request_t *req, uint32_t timeout_ms)
{
    if (!s_direct_copy.pool || req->src_color_format != req->dst_color_format) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t bit_depth = color_hal_pixel_format_fourcc_get_bit_depth(req->src_color_format);
    if (bit_depth != 16 && bit_depth != 24 && bit_depth != 32) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (req->copy_width == 0 || req->copy_height == 0 ||
            req->src_stride > DMA2D_LL_DESC_2D_FIELD_MAX || req->src_height > DMA2D_LL_DESC_2D_FIELD_MAX ||
            req->dst_stride > DMA2D_LL_DESC_2D_FIELD_MAX || req->dst_height > DMA2D_LL_DESC_2D_FIELD_MAX ||
            req->src_x + req->copy_width > req->src_stride || req->src_y + req->copy_height > req->src_height ||
            req->dst_x + req->copy_width > req->dst_stride || req->dst_y + req->copy_height > req->dst_height) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t bytes_per_pixel = bit_depth / 8;
    uint32_t pbyte = dma2d_desc_pixel_format_to_pbyte_value(req->src_color_format);
    s_direct_copy_setup_desc(s_direct_copy.tx_desc, req->src_buffer, req->src_stride, req->src_height, req->src_x,
                             req->src_y, req->copy_width, req->copy_height, pbyte);
    s_direct_copy_setup_desc(s_direct_copy.rx_desc, req->dst_buffer, req->dst_stride, req->dst_height, req->dst_x,
                             req->dst_y, req->copy_width, req->copy_height, pbyte);
    ESP_RETURN_ON_ERROR(s_direct_copy_sync_rows(req->src_buffer, (size_t)req->src_stride * bytes_per_pixel,
                                                req->src_x, req->src_y, req->copy_width, req->copy_height,
                                                bytes_per_pixel, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                        TAG, "source cache sync failed");
    ESP_RETURN_ON_ERROR(s_direct_copy_sync_rows(req->dst_buffer, (size_t)req->dst_stride * bytes_per_pixel,
                                                req->dst_x, req->dst_y, req->copy_width, req->copy_height,
                                                bytes_per_pixel,
                                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
                        TAG, "destination cache sync failed");
    ESP_RETURN_ON_ERROR(esp_cache_msync(s_direct_copy.tx_desc, s_direct_copy.desc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                        TAG, "descriptor cache sync failed");
    ESP_RETURN_ON_ERROR(esp_cache_msync(s_direct_copy.rx_desc, s_direct_copy.desc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                        TAG, "descriptor cache sync failed");
    ESP_RETURN_ON_ERROR(dma2d_enqueue(s_direct_copy.pool, &s_direct_copy.trans_config, s_direct_copy.trans_placeholder),
                        TAG, "DMA2D enqueue failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake((SemaphoreHandle_t)s_hw_resource.dma2d_done_sem,
                                       pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "DMA2D transfer timeout");
    return ESP_OK;
}
#endif /* CONFIG_SOC_DMA2D_SUPPORTED && ESP_ASYNC_COLOR_CONVERT_AVAILABLE */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
#include "esp_private/sleep_retention.h"
#endif

#if CONFIG_SOC_DMA2D_SUPPORTED
static inline esp_err_t s_dma2d_install(void **out_handle)
{
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
    async_color_convert_config_t cfg = {
        .dma_burst_size = 128,
    };
    return esp_async_color_convert_install_dma2d(&cfg, (async_color_convert_handle_t *)out_handle);
#else
    esp_async_fbcpy_config_t cfg = {};
    return esp_async_fbcpy_install(&cfg, (esp_async_fbcpy_handle_t *)out_handle);
#endif
}

static inline void s_dma2d_uninstall(void *handle)
{
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
    esp_async_color_convert_uninstall((async_color_convert_handle_t)handle);
#else
    esp_async_fbcpy_uninstall((esp_async_fbcpy_handle_t)handle);
#endif
}
#endif /* CONFIG_SOC_DMA2D_SUPPORTED */
#endif /* SOC_DMA2D_SUPPORTED */

/**
 * @brief Initialize runtime information from configuration
 *
 * This function is 100% identical in both v8 and v9 implementations
 */
esp_err_t display_bridge_init_runtime_info(esp_lv_adapter_display_runtime_info_t *runtime,
                                           const esp_lv_adapter_display_runtime_config_t *cfg)
{
    if (!runtime || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    runtime->hor_res = cfg->base.profile.hor_res;
    runtime->ver_res = cfg->base.profile.ver_res;
    runtime->rotation = cfg->base.profile.rotation;
    runtime->frame_buffer_count = cfg->frame_buffer_count;
    runtime->frame_buffer_size = cfg->frame_buffer_size;

    for (int i = 0; i < 3; i++) {
        runtime->frame_buffers[i] = cfg->frame_buffers[i];
    }

    /* Derive panel framebuffer pixel size from the configured framebuffer geometry. */
    size_t total_pixels = (size_t)runtime->hor_res * runtime->ver_res;
    uint8_t color_bytes = 0;
    uint8_t lvgl_color_bytes = 0;

    if (runtime->frame_buffer_size && total_pixels) {
        if ((runtime->frame_buffer_size % total_pixels) != 0 &&
                cfg->base.profile.mono_layout == ESP_LV_ADAPTER_MONO_LAYOUT_NONE) {
            ESP_LOGE(TAG, "frame_buffer_size=%zu is not pixel-aligned for %ux%u panel",
                     runtime->frame_buffer_size, runtime->hor_res, runtime->ver_res);
            return ESP_ERR_INVALID_ARG;
        }
        color_bytes = runtime->frame_buffer_size / total_pixels;
    }

    if (LV_COLOR_DEPTH % 8 == 0) {
        lvgl_color_bytes = LV_COLOR_DEPTH / 8;
    } else if (cfg->base.profile.mono_layout == ESP_LV_ADAPTER_MONO_LAYOUT_NONE) {
        ESP_LOGE(TAG, "LV_COLOR_DEPTH=%d is not supported for non-monochrome display", LV_COLOR_DEPTH);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->base.profile.mono_layout == ESP_LV_ADAPTER_MONO_LAYOUT_NONE &&
            color_bytes && lvgl_color_bytes && color_bytes != lvgl_color_bytes) {
        ESP_LOGE(TAG, "LVGL/LCD color depth mismatch: LV_COLOR_DEPTH=%d (%u bytes), framebuffer_size=%zu, panel=%ux%u, panel_color_bytes=%u",
                 LV_COLOR_DEPTH, lvgl_color_bytes, runtime->frame_buffer_size,
                 runtime->hor_res, runtime->ver_res, color_bytes);
        return ESP_ERR_INVALID_ARG;
    }

    if (color_bytes == 0) {
        color_bytes = lvgl_color_bytes;
    }
    if (color_bytes == 0) {
        color_bytes = sizeof(lv_color_t);
    }

    /* Ensure color_bytes has a valid value (final fallback to RGB565) */
    if (color_bytes == 0) {
        color_bytes = 2;  /* RGB565: 2 bytes per pixel */
        ESP_LOGW(TAG, "Unable to determine color bytes, defaulting to RGB565 (2 bytes)");
    }

    runtime->color_bytes = color_bytes;

    /* Recalculate frame_buffer_size if it wasn't set */
    if (runtime->frame_buffer_size == 0 && total_pixels && color_bytes) {
        runtime->frame_buffer_size = total_pixels * color_bytes;
        ESP_LOGD(TAG, "Calculated frame_buffer_size: %zu bytes (%zux%u pixels, %u bpp)",
                 runtime->frame_buffer_size, total_pixels, color_bytes * 8);
    }

    if (runtime->color_bytes != 2 && runtime->color_bytes != 3 &&
            cfg->base.profile.mono_layout == ESP_LV_ADAPTER_MONO_LAYOUT_NONE) {
        ESP_LOGW(TAG, "color depth %u bytes not HW-accelerated; using CPU fallback",
                 runtime->color_bytes);
    }

    return ESP_OK;
}

#if SOC_DMA2D_SUPPORTED

/* Global hardware resource - shared between v8 and v9 (defined above the
 * direct DMA2D copy helpers, which signal its completion semaphore). */
static bool s_hw_resource_initialized = false;
static uint8_t s_hw_resource_ref_count = 0;
static bool s_hw_resource_sleep_guard_active = false;

/**
 * @brief Get global hardware resource singleton
 *
 * This function implements reference counting for hardware resources.
 * Multiple displays can share the same PPA and DMA2D hardware.
 */
esp_lv_adapter_display_bridge_hw_resource_t *display_bridge_get_hw_resource(void)
{
    if (!s_hw_resource_initialized) {
        ESP_LOGI(TAG, "Initializing hardware resources");

#if CONFIG_SOC_PPA_SUPPORTED
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_hw_resource.data_cache_line_size);
        if (s_hw_resource.data_cache_line_size == 0) {
            /* Default PPA alignment: 128 bytes for cache line optimization */
            s_hw_resource.data_cache_line_size = 128;
        }
#endif

#if CONFIG_SOC_DMA2D_SUPPORTED
        esp_err_t ret = s_dma2d_install(&s_hw_resource.fbcpy_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install DMA2D (ret=%d)", ret);
            return NULL;
        }

        s_hw_resource.dma2d_mutex = xSemaphoreCreateMutex();
        s_hw_resource.dma2d_done_sem = xSemaphoreCreateBinary();

        if (!s_hw_resource.dma2d_mutex || !s_hw_resource.dma2d_done_sem) {
            ESP_LOGE(TAG, "Failed to create DMA2D semaphores");
            /* Cleanup on failure */
            if (s_hw_resource.dma2d_mutex) {
                vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_mutex);
            }
            if (s_hw_resource.dma2d_done_sem) {
                vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_done_sem);
            }
            s_dma2d_uninstall(s_hw_resource.fbcpy_handle);
            return NULL;
        }
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
        s_direct_copy_install();
#endif
#endif

#if CONFIG_SOC_PPA_SUPPORTED
        if (s_hw_resource.ppa_handle == NULL) {
            ppa_client_config_t ppa_srm_config = {
                .oper_type = PPA_OPERATION_SRM,
                .max_pending_trans_num = LVGL_PORT_PPA_MAX_PENDING_TRANS,
            };
            ret = ppa_register_client(&ppa_srm_config,
                                      (ppa_client_handle_t *)&s_hw_resource.ppa_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register PPA client (ret=%d)", ret);
#if CONFIG_SOC_DMA2D_SUPPORTED
                /* Cleanup DMA2D resources */
                vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_mutex);
                vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_done_sem);
                s_dma2d_uninstall(s_hw_resource.fbcpy_handle);
#endif
                return NULL;
            }
        }
#endif

        s_hw_resource_initialized = true;
        ESP_LOGI(TAG, "Hardware resources initialized successfully");
    }

    /* Increment reference count */
    s_hw_resource_ref_count++;
    ESP_LOGD(TAG, "Hardware resource acquired (ref_count=%d)", s_hw_resource_ref_count);

    return &s_hw_resource;
}

esp_lv_adapter_display_bridge_hw_resource_t *display_bridge_peek_hw_resource(void)
{
    return s_hw_resource_initialized ? &s_hw_resource : NULL;
}

/**
 * @brief Release hardware resource reference
 *
 * Decrements the reference count and cleans up hardware resources
 * when the count reaches zero.
 */
esp_err_t display_bridge_release_hw_resource(void)
{
    if (!s_hw_resource_initialized) {
        ESP_LOGW(TAG, "Hardware resources not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_hw_resource_ref_count == 0) {
        ESP_LOGW(TAG, "Hardware resource reference count already zero");
        return ESP_ERR_INVALID_STATE;
    }

    /* Decrement reference count */
    s_hw_resource_ref_count--;
    ESP_LOGD(TAG, "Hardware resource released (ref_count=%d)", s_hw_resource_ref_count);

    /* Cleanup when no more references */
    if (s_hw_resource_ref_count == 0) {
        ESP_LOGI(TAG, "Cleaning up hardware resources");

#if CONFIG_SOC_PPA_SUPPORTED
        if (s_hw_resource.ppa_handle) {
            ppa_unregister_client((ppa_client_handle_t)s_hw_resource.ppa_handle);
            s_hw_resource.ppa_handle = NULL;
            ESP_LOGI(TAG, "PPA client unregistered");
        }
#endif

#if CONFIG_SOC_DMA2D_SUPPORTED
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
        s_direct_copy_release();
#endif
        if (s_hw_resource.dma2d_mutex) {
            vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_mutex);
            s_hw_resource.dma2d_mutex = NULL;
        }

        if (s_hw_resource.dma2d_done_sem) {
            vSemaphoreDelete((SemaphoreHandle_t)s_hw_resource.dma2d_done_sem);
            s_hw_resource.dma2d_done_sem = NULL;
        }

        if (s_hw_resource.fbcpy_handle) {
            if (s_hw_resource_sleep_guard_active) {
#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
                sleep_retention_power_lock_release();
#endif
                s_hw_resource_sleep_guard_active = false;
            }
            s_dma2d_uninstall(s_hw_resource.fbcpy_handle);
            s_hw_resource.fbcpy_handle = NULL;
            ESP_LOGI(TAG, "DMA2D resources released");
        }
#endif

        s_hw_resource_initialized = false;
        s_hw_resource_sleep_guard_active = false;
        memset(&s_hw_resource, 0, sizeof(s_hw_resource));
        ESP_LOGI(TAG, "Hardware resources cleaned up successfully");
    }

    return ESP_OK;
}

/**
 * @brief DMA2D completion callback (ISR context)
 *
 * IRAM: Executed in ISR context for fast response
 */
#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
bool IRAM_ATTR display_bridge_dma2d_done_callback(async_color_convert_handle_t mcp,
                                                  async_color_convert_event_data_t *event_data,
                                                  void *cb_args)
#else
bool IRAM_ATTR display_bridge_dma2d_done_callback(esp_async_fbcpy_handle_t mcp,
                                                  esp_async_fbcpy_event_data_t *event_data,
                                                  void *cb_args)
#endif
{
    BaseType_t high_task_woken = pdFALSE;
    (void)mcp;
    (void)event_data;
    (void)cb_args;

    esp_lv_adapter_display_bridge_hw_resource_t *hw = &s_hw_resource;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)hw->dma2d_done_sem, &high_task_woken);

    return (high_task_woken == pdTRUE);
}

/**
 * @brief Synchronous DMA2D copy operation
 *
 * Note: Mutex held during entire transfer to serialize access to shared
 * dma2d_done_sem semaphore (global resource shared by all displays)
 */
esp_err_t display_bridge_dma2d_copy_sync(void *trans_desc, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    esp_lv_adapter_display_bridge_hw_resource_t *hw = display_bridge_peek_hw_resource();
    ESP_RETURN_ON_FALSE(hw, ESP_ERR_INVALID_STATE, TAG, "DMA2D resource not initialized");

    ESP_GOTO_ON_FALSE(xSemaphoreTake((SemaphoreHandle_t)hw->dma2d_mutex,
                                     pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
                      ESP_ERR_TIMEOUT, out, TAG, "Acquire DMA2D mutex timeout");

    /* Clear completion semaphore */
    xSemaphoreTake((SemaphoreHandle_t)hw->dma2d_done_sem, 0);

#ifdef ESP_ASYNC_COLOR_CONVERT_AVAILABLE
    ret = s_direct_copy_run((const async_color_convert_request_t *)trans_desc, timeout_ms);
    if (ret != ESP_ERR_NOT_SUPPORTED) {
        goto release_mutex;
    }
    ret = esp_async_color_convert(hw->fbcpy_handle,
                                  (const async_color_convert_request_t *)trans_desc,
                                  display_bridge_dma2d_done_callback,
                                  NULL);
#else
    ret = esp_async_fbcpy(hw->fbcpy_handle,
                          trans_desc,
                          display_bridge_dma2d_done_callback,
                          NULL);
#endif
    ESP_GOTO_ON_ERROR(ret, release_mutex, TAG, "DMA2D transfer start failed (%d)", ret);

    ESP_GOTO_ON_FALSE(xSemaphoreTake((SemaphoreHandle_t)hw->dma2d_done_sem,
                                     pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
                      ESP_ERR_TIMEOUT, release_mutex, TAG, "DMA2D transfer timeout");

release_mutex:
    xSemaphoreGive((SemaphoreHandle_t)hw->dma2d_mutex);

out:
    return ret;
}

#endif /* SOC_DMA2D_SUPPORTED */

/**
 * @brief Prepare shared bridge HW resources for a board-managed light sleep cycle
 *
 * Defined unconditionally and a no-op on targets without DMA2D. When DMA2D is
 * present and peripheral power-down is enabled, it forces the peripheral power
 * domain to stay on across sleep, since 2D-DMA has no sleep retention.
 */
esp_err_t display_bridge_prepare_hw_resource_for_sleep(void)
{
#if CONFIG_SOC_DMA2D_SUPPORTED
    if (!s_hw_resource_initialized || s_hw_resource_sleep_guard_active) {
        return ESP_OK;
    }

#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    ESP_LOGI(TAG, "Guarding DMA2D bridge resources for light sleep");
    ESP_RETURN_ON_ERROR(sleep_retention_power_lock_acquire(), TAG,
                        "Failed to acquire sleep retention power lock");
    s_hw_resource_sleep_guard_active = true;
    ESP_LOGI(TAG, "DMA2D bridge resources guarded for light sleep");
#endif
#endif

    return ESP_OK;
}

/**
 * @brief Release any shared bridge HW guard acquired for a light sleep cycle
 *
 * Defined unconditionally and safe to call even when no guard was activated
 * during sleep preparation (including on targets without DMA2D).
 */
esp_err_t display_bridge_resume_hw_resource_after_sleep(void)
{
#if CONFIG_SOC_DMA2D_SUPPORTED
    if (!s_hw_resource_sleep_guard_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Releasing DMA2D light-sleep guard");
#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP
    ESP_RETURN_ON_ERROR(sleep_retention_power_lock_release(), TAG,
                        "Failed to release sleep retention power lock");
#endif
    s_hw_resource_sleep_guard_active = false;
    ESP_LOGI(TAG, "DMA2D light-sleep guard released");
#endif

    return ESP_OK;
}

/**
 * @brief Destroy display bridge and release resources
 *
 * This function is shared between v8 and v9 implementations.
 * It releases hardware resources (if any) and frees the bridge structure.
 *
 * @param bridge Display bridge structure to destroy
 */
void display_bridge_common_destroy(esp_lv_adapter_display_bridge_t *bridge)
{
    /* NULL pointer check - safe to call with NULL */
    if (!bridge) {
        return;
    }

#if CONFIG_SOC_DMA2D_SUPPORTED || CONFIG_SOC_PPA_SUPPORTED
    /* Release shared hardware resources with reference counting */
    display_bridge_release_hw_resource();
#endif

    /* Free the bridge implementation structure */
    free(bridge);
}

/**********************
 *   PIPELINE HELPERS
 **********************/

int display_bridge_pipeline_init_from_cfg(esp_lv_adapter_display_pipeline_t *pipeline,
                                          esp_lv_adapter_display_runtime_config_t *cfg,
                                          void **out_disp_fb, void **out_draw_fb)
{
    int ret = 1;

    if (!pipeline || !cfg || !out_disp_fb || !out_draw_fb) {
        return 0;
    }
    *out_disp_fb = NULL;
    *out_draw_fb = NULL;

    const esp_lv_adapter_tear_avoid_mode_t mode = cfg->base.tear_avoid_mode;
    const bool rotated = (cfg->base.profile.rotation != ESP_LV_ADAPTER_ROTATE_0);
    const bool use_pipeline = (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL ||
                               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL ||
                               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT ||
                               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL ||
                               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL);
    if (!use_pipeline || cfg->frame_buffer_count == 0) {
        return 0;
    }

    void **fb = cfg->frame_buffers;
    uint8_t fb_count = cfg->frame_buffer_count;

    pipeline->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    STAILQ_INIT(&pipeline->busy_list);
    STAILQ_INIT(&pipeline->empty_list);
    pipeline->elem_count = fb_count;
    pipeline->elems = calloc(fb_count, sizeof(struct display_pipeline_buf));
    if (!pipeline->elems) {
        ESP_LOGE(TAG, "Failed to alloc pipeline elements");
        return -1;
    }
    for (int i = 0; i < fb_count; i++) {
        pipeline->elems[i].buffer = fb[i];
    }

    if (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL ||
            mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL) {
        /* partial: independent buffers used for draw; fb[0],fb[1], fb[2..] in empty_list */
        uint8_t required = (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL) ? 3 : 2;
        if (fb_count < required) {
            ESP_LOGE(TAG, "PARTIAL mode %d requires %d frame buffers, got %d", mode, required, fb_count);
            ret = -1;
            goto err;
        }
        *out_disp_fb = fb[0];
        *out_draw_fb = fb[1];
        for (int i = 2; i < fb_count; i++) {
            STAILQ_INSERT_TAIL(&pipeline->empty_list, &pipeline->elems[i], entry);
        }
    } else if (rotated) {
        /*
         * rotated non-partial:
         *   fb[0]=render, fb[1]=disp, fb[2]=draw
         *
         * model:
         *   render path: fb[0] -> fb[2]
         *   panel ring : fb[1] <-> fb[2]
         */
        if (fb_count < 3) {
            ESP_LOGE(TAG, "Rotated mode %d requires 3 frame buffers, got %d", mode, fb_count);
            ret = -1;
            goto err;
        }
        *out_disp_fb = fb[1];
        *out_draw_fb = fb[2];
    } else if (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT ||
               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL ||
               mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL) {
        uint8_t required = (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL) ? 3 : 2;
        if (fb_count < required) {
            ESP_LOGE(TAG, "FULL mode %d requires %d frame buffers, got %d", mode, required, fb_count);
            ret = -1;
            goto err;
        }

        *out_disp_fb = fb[0];
        *out_draw_fb = fb[1];
        int free_start = (mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL) ? 2 : 1;
        for (int i = free_start; i < fb_count; i++) {
            STAILQ_INSERT_TAIL(&pipeline->empty_list, &pipeline->elems[i], entry);
        }
    }

    return 1;

err:
    free(pipeline->elems);
    pipeline->elems = NULL;
    pipeline->elem_count = 0;
    return ret;
}

void display_bridge_pipeline_mark_buf_busy(esp_lv_adapter_display_pipeline_t *p, void *buf)
{
    if (!p) {
        return;
    }
    for (int i = 0; i < p->elem_count; i++) {
        if (p->elems[i].buffer == buf) {
            portENTER_CRITICAL(&p->lock);
            STAILQ_INSERT_TAIL(&p->busy_list, &p->elems[i], entry);
            portEXIT_CRITICAL(&p->lock);
            return;
        }
    }
}

void IRAM_ATTR display_bridge_pipeline_release_buf_isr(esp_lv_adapter_display_pipeline_t *p)
{
    if (!p || !p->elems) {
        return;
    }
    portENTER_CRITICAL_ISR(&p->lock);
    struct display_pipeline_buf *elem = STAILQ_FIRST(&p->busy_list);
    if (elem) {
        STAILQ_REMOVE_HEAD(&p->busy_list, entry);
        STAILQ_INSERT_TAIL(&p->empty_list, elem, entry);
    }
    portEXIT_CRITICAL_ISR(&p->lock);
}

struct display_pipeline_buf *display_bridge_pipeline_take_free_buf(esp_lv_adapter_display_pipeline_t *p)
{
    struct display_pipeline_buf *next = NULL;
    if (!p) {
        return NULL;
    }
    portENTER_CRITICAL(&p->lock);
    next = STAILQ_FIRST(&p->empty_list);
    if (next) {
        STAILQ_REMOVE_HEAD(&p->empty_list, entry);
    }
    portEXIT_CRITICAL(&p->lock);
    return next;
}

struct display_pipeline_buf *display_bridge_pipeline_wait_free_buf(esp_lv_adapter_display_pipeline_t *p)
{
    if (!p) {
        return NULL;
    }

    /* Clear any stale notification BEFORE checking the list,
     * so that an ISR firing after this point will not be lost. */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    struct display_pipeline_buf *next = display_bridge_pipeline_take_free_buf(p);
    while (!next) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        next = display_bridge_pipeline_take_free_buf(p);
        if (!next) {
            ESP_LOGW(TAG, "without free buffer");
        }
    }
    return next;
}

/**********************
 *   VSYNC HELPERS
 **********************/

void display_bridge_vsync_record_flush_post(esp_lv_adapter_vsync_timing_t *t)
{
    if (t) {
        t->flush_post_ts_us = esp_timer_get_time();
    }
}

bool IRAM_ATTR display_bridge_vsync_on_isr(esp_lv_adapter_vsync_timing_t *t)
{
    if (!t) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();

    /* Update measured refresh period */
    if (t->vsync_prev_ts_us != 0) {
        const int64_t interval_us = now_us - t->vsync_prev_ts_us;
        if (interval_us > 0) {
            t->refresh_period_us = (uint32_t)interval_us;
        }
    }
    t->vsync_prev_ts_us = now_us;

    /* Jitter Shield Logic */
    if (t->shield_enabled && t->refresh_period_us != 0 && t->flush_post_ts_us != 0) {
        const int64_t dt = now_us - t->flush_post_ts_us;
        if (dt > 0 && (uint64_t)dt < (uint64_t)t->shield_threshold_us) {
            return true;
        }
    }
    return false;
}

void display_bridge_vsync_config_jitter_shield(esp_lv_adapter_vsync_timing_t *t, uint32_t threshold_us)
{
    if (t) {
        t->shield_threshold_us = threshold_us;
        t->shield_enabled = (threshold_us > 0);
    }
}

uint32_t display_bridge_vsync_get_refresh_rate_hz(esp_lv_adapter_vsync_timing_t *t)
{
    if (!t || t->refresh_period_us == 0) {
        return 0;
    }
    return 1000000U / t->refresh_period_us;
}
