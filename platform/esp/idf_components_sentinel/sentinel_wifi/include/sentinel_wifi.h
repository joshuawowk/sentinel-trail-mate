#pragma once

/*
 * The link to the Tab5's ESP32-C6 under Sentinel Bootloader.
 *
 * The C6 runs Sentinel's shared esp-hosted slave firmware. esp_hosted starts its transport
 * before app_main; sentinel_wifi_link_up() connects to the slave (which resets the C6, as the
 * launcher and every Sentinel app do at start) so that esp_wifi_init() can follow. Nothing here
 * writes the C6's flash.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * Connect to the esp-hosted slave once. Blocks while the C6 resets and its SDIO link comes up
     * (about two seconds; up to about 20 s when it does not answer). Returns true when the link is
     * up. After a failure, later calls try again at most every 30 s and return false meanwhile.
     */
    bool sentinel_wifi_link_up(void);

    /* True once sentinel_wifi_link_up() succeeded. */
    bool sentinel_wifi_link_ready(void);

    /* The slave firmware's version ("2.12.16"), or "" before the link is up. */
    void sentinel_wifi_c6_version(char* out, size_t len);

    /*
     * Before handing the Tab5 back to the launcher: leave the network and stop the station, as
     * Sentinel apps do, so the access point sees a clean disassociation. Does nothing when the link
     * never came up. Safe to call from any task.
     */
    void sentinel_wifi_stop_for_launch(void);

#ifdef __cplusplus
}
#endif
