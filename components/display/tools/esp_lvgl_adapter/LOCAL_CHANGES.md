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
