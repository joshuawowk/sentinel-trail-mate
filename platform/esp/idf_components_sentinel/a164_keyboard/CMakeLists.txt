# M5Stack Tab5 A164 I2C keyboard (addr 0x6D on GPIO0/1, ExtPort1) as an LVGL
# KEYPAD input device. Only the Sentinel build adds this directory (see the root
# CMakeLists.txt EXTRA_COMPONENT_DIRS), so the standalone Tab5 and T-Display-P4
# builds never compile or link it. Ported from the user's M5MonsterC5-Tab5 repo.
idf_component_register(
    SRCS "a164_keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver lvgl esp_lvgl_port
)
