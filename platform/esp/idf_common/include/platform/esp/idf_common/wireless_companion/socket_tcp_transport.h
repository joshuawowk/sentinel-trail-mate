#pragma once

#include "platform/esp/idf_common/wireless_companion/c6_companion.h"

namespace platform::esp::idf_common::wireless_companion
{

// One TCP connection over the P4's own lwIP stack, for builds whose Wi-Fi is a network
// interface on the P4 (the Sentinel build: esp-hosted on the C6, esp_wifi_remote here)
// instead of Trail Mate's HostLink TCP proxy on the C6. Same contract as the HostLink
// transport: openTcp() returns at once and the connection completes in the background
// (Opening, then Connected or Error); readTcp() never blocks; a remote close leaves the
// unread bytes readable and then reports Disconnected.
WifiTcpTransport& socket_tcp_transport();

} // namespace platform::esp::idf_common::wireless_companion
