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
