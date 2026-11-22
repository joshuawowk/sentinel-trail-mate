# M5Stack Tab5 Keyboard (SKU A164): I2C 0x6D on GPIO0/1 (expansion port J9). Raw driver
# only -- the LVGL side is platform/esp/idf_common/src/sentinel_keyboard.cpp. Only the
# Sentinel build adds this directory (see the root CMakeLists.txt EXTRA_COMPONENT_DIRS), so
# the standalone Tab5 and T-Display-P4 builds never compile or link it.
idf_component_register(
    SRCS "a164_keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_timer
)
