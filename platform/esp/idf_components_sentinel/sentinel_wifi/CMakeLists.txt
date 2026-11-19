# Wi-Fi through the Tab5's ESP32-C6 under Sentinel Bootloader: esp-hosted over SDIO slot 1,
# with esp_wifi_remote providing the esp_wifi API on the ESP32-P4. Only the Sentinel build adds
# this directory (see the root CMakeLists.txt), so the standalone Tab5 and T-Display-P4 builds,
# whose C6 speaks Trail Mate's HostLink protocol, never link esp_hosted.
idf_component_register(SRCS "sentinel_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event lwip
                       PRIV_REQUIRES esp_hosted esp_wifi_remote freertos log m5stack_tab5)
