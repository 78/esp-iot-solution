# Local changes

This component is based on Espressif `esp_lvgl_adapter` 0.6.3 at commit
`db9f2f48527f9a98a87e0ca741c8dacd31814f42`.

MetalioClaw adds two framebuffer-preserving APIs used by
`RetainedSurface` during translated-surface composition:

- `esp_lv_adapter_dummy_draw_get_free_buf_preserve()` acquires a free panel
  framebuffer without performing the adapter's lazy clear.
- `esp_lv_adapter_disable_dummy_draw_preserve_content()` restores LVGL's panel
  framebuffer bindings and leaves dummy-draw mode without requesting a full
  screen refresh.

These APIs are safe only when the caller tracks and restores every panel
framebuffer's contents. The regular adapter APIs remain the fallback for an
incomplete restore.

The fork also adds `ESP_LV_ADAPTER_TICK_MODE_MONOTONIC` for LVGL 9 products.
This mode uses LVGL's tick callback to read the ESP-IDF monotonic clock on
demand, eliminating the adapter's periodic tick timer. The worker separately
bounds its dynamic LVGL deadline by the auto-sleep entry deadline.

The full display sleep flow can also start while pause-mode auto sleep is
active. `esp_lv_adapter_sleep_prepare()` first exits the adapter-owned idle
pause, restoring its PM lock and display bridge guard, and then performs the
normal detach sequence. A caller-owned manual pause remains an invalid state.

Optional `CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY` counters expose
aggregated RGB888 software-image, framebuffer-sync, panel-submit and VSYNC-wait
costs. The option defaults off and adds no timing calls to production builds.

On RGB888 displays the LVGL v9 PPA blend handler copies opaque RGB888 image
blocks of at least 1024 pixels with DMA2D through the shared bridge copy handle
instead of the CPU blend (`lvgl_ppa_accel_v9.c`, `ppa_v9_dma2d_copy_rgb888`).
The custom-handler path already restricts the block to the source area, so the
copy is exact for tiled images as well. Sources in flash, misaligned windows
under flash encryption and small blocks keep the CPU path. This removes the
~137 ns/px CPU cost of presenting a PSRAM App Surface into the framebuffer.

`display_bridge_dma2d_copy_sync()` runs same-format 16/24/32-bit copies on
bridge-owned 2D-DMA descriptors (`display_bridge_common.c`, `s_direct_copy_*`)
instead of `esp_async_color_convert`, which writes back the whole source picture
and writes back + invalidates the whole destination picture on every request
(about 3 MB of cache maintenance per copy for a 720x720 RGB888 frame). The
direct path only synchronizes the rows a block touches, the same way the PPA
driver handles its extended windows. Format conversions, or a failed pool or
descriptor allocation, keep using the helper. This applies to framebuffer
dirty-area sync, flush blits and the opaque RGB888 image copy above.
