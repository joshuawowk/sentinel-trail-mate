# sentinel_app

`include/sentinel_app.h` is vendored unchanged from
[joshuawowk/sentinel-bootloader](https://github.com/joshuawowk/sentinel-bootloader)
(`sdk/sentinel_app/include/sentinel_app.h`, branch `feat/repo-updates`, commit 75d14c2),
MIT licensed.

It defines the 256-byte `sentinel_app_info_t` record that Sentinel Bootloader reads at image
offset `0x120` (right after `esp_app_desc_t`, in `.rodata_custom_desc`) to learn which
GitHub repository and release asset a firmware updates from. Trail Mate's Sentinel build
places it in `platform/esp/idf_common/src/sentinel_runtime.cpp` and links with
`-Wl,-u,sentinel_app_info` so the linker keeps it.
