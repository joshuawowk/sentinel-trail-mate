#include "platform/esp/idf_common/wireless_companion/socket_tcp_transport.h"

#include "hostlink/c6/c6_protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

namespace platform::esp::idf_common::wireless_companion
{
namespace
{

constexpr const char* kTag = "TCP";
constexpr int kConnectTimeoutSeconds = 10;
constexpr int kSendTimeoutSeconds = 5;
constexpr uint32_t kWorkerStackBytes = 6144;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 5;

struct OpenRequest
{
    uint32_t generation = 0;
    uint16_t port = 0;
    char host[TM_C6_WIFI_TCP_HOST_LEN] = {};
};

bool station_has_address()
{
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    return sta && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

void tune_connected_socket(int fd)
{
    // Writes block for at most kSendTimeoutSeconds; reads are always non-blocking (MSG_DONTWAIT).
    timeval send_timeout{};
    send_timeout.tv_sec = kSendTimeoutSeconds;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Reticulum gateway links stay open for hours: notice a dead peer within a few minutes.
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    int idle = 60, interval = 15, count = 4;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

// DNS and a connect with a timeout; returns the socket or -1 with `detail` and `error` set.
int connect_endpoint(const OpenRequest& request, const char*& detail, uint16_t& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_text[8];
    std::snprintf(port_text, sizeof(port_text), "%u", static_cast<unsigned>(request.port));
    addrinfo* results = nullptr;
    if (getaddrinfo(request.host, port_text, &hints, &results) != 0 || results == nullptr)
    {
        detail = "dns_failed";
        error = TM_C6_ERROR_NOT_CONNECTED;
        return -1;
    }

    int fd = -1;
    detail = "connect_failed";
    error = TM_C6_ERROR_NOT_CONNECTED;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next)
    {
        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0)
        {
            detail = "socket_failed";
            error = TM_C6_ERROR_LOW_MEMORY;
            continue;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        bool connected = connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS)
        {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval timeout{};
            timeout.tv_sec = kConnectTimeoutSeconds;
            const int ready = select(fd + 1, nullptr, &writable, nullptr, &timeout);
            if (ready > 0)
            {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0;
            }
            else if (ready == 0)
            {
                detail = "connect_timeout";
                error = TM_C6_ERROR_TIMEOUT;
            }
        }
        if (connected)
        {
            (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            tune_connected_socket(fd);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    return fd;
}

class SocketTcpTransport final : public WifiTcpTransport
{
  public:
    SocketTcpTransport()
    {
        status_.state = WifiTcpState::Disconnected;
        status_.detail = "not_connected";
    }

    bool openTcp(const char* host, uint16_t port) override
    {
        if (host == nullptr || host[0] == '\0' || std::strlen(host) >= TM_C6_WIFI_TCP_HOST_LEN || port == 0)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_; // invalidate any in-flight open so it can't publish Connected later
            close_locked();
            set_locked(WifiTcpState::Error, TM_C6_ERROR_INTERNAL, "invalid_endpoint");
            return false;
        }
        if (!station_has_address())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            close_locked();
            set_locked(WifiTcpState::Disconnected, TM_C6_ERROR_NOT_CONNECTED, "wifi_not_ready");
            return false;
        }
        if (!ensure_worker())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            set_locked(WifiTcpState::Error, TM_C6_ERROR_LOW_MEMORY, "worker_unavailable");
            return false;
        }

        OpenRequest request{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_locked();
            ++generation_;
            status_ = WifiTcpStatus{};
            status_.connection_id = static_cast<uint8_t>(generation_);
            set_locked(WifiTcpState::Opening, TM_C6_OK, "opening");
            request.generation = generation_;
        }
        request.port = port;
        std::strncpy(request.host, host, sizeof(request.host) - 1);
        // A newer open replaces one the worker has not started yet.
        (void)xQueueOverwrite(queue_, &request);
        return true;
    }

    bool writeTcp(const uint8_t* data, size_t len) override
    {
        if (data == nullptr || len == 0)
        {
            return false;
        }
        // Held across send() so a closeTcp() from another task (the Wi-Fi runtime disabling the
        // transport on the console or LVGL task, while Reticulum writes on the chat task) waits
        // for the write to finish instead of closing the fd under it and risking fd reuse.
        // send() blocks for at most SO_SNDTIMEO; a chunked loop keeps any single call short.
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != WifiTcpState::Connected || fd_ < 0)
        {
            return false;
        }
        size_t offset = 0;
        while (offset < len)
        {
            const ssize_t sent = send(fd_, data + offset, len - offset, 0);
            if (sent > 0)
            {
                offset += static_cast<size_t>(sent);
                continue;
            }
            const int send_errno = errno;
            if (sent < 0 && send_errno == EINTR)
            {
                continue;
            }
            close_locked();
            const bool timed_out = send_errno == EAGAIN || send_errno == EWOULDBLOCK;
            set_locked(WifiTcpState::Error, timed_out ? TM_C6_ERROR_TIMEOUT : TM_C6_ERROR_NOT_CONNECTED,
                       timed_out ? "write_timeout" : "write_failed");
            return false;
        }
        status_.transmitted_bytes += static_cast<uint32_t>(len);
        return true;
    }

    size_t readTcp(uint8_t* out, size_t max_len) override
    {
        if (out == nullptr || max_len == 0)
        {
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ < 0)
        {
            return 0;
        }
        const ssize_t received = recv(fd_, out, max_len, MSG_DONTWAIT);
        if (received > 0)
        {
            status_.received_bytes += static_cast<uint32_t>(received);
            refresh_buffered_locked();
            return static_cast<size_t>(received);
        }
        if (received == 0)
        {
            close_locked();
            set_locked(WifiTcpState::Disconnected, TM_C6_OK, "remote_closed");
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return 0;
        }
        close_locked();
        set_locked(WifiTcpState::Error, TM_C6_ERROR_NOT_CONNECTED, "read_failed");
        return 0;
    }

    void closeTcp() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_; // an open still in progress closes its socket when it finishes
        const bool idle = status_.state == WifiTcpState::Disconnected || status_.state == WifiTcpState::Unsupported;
        close_locked();
        if (!idle)
        {
            set_locked(WifiTcpState::Disconnected, TM_C6_OK, "closed_locally");
        }
    }

    WifiTcpStatus tcpStatus() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == WifiTcpState::Connected && fd_ >= 0)
        {
            refresh_buffered_locked();
            if (status_.buffered_bytes == 0)
            {
                // Nothing to read: has the peer closed? (The HostLink transport reports that
                // without a read, and callers watch the state.)
                uint8_t probe = 0;
                const ssize_t peeked = recv(fd_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peeked == 0)
                {
                    close_locked();
                    set_locked(WifiTcpState::Disconnected, TM_C6_OK, "remote_closed");
                }
                else if (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    close_locked();
                    set_locked(WifiTcpState::Error, TM_C6_ERROR_NOT_CONNECTED, "connection_lost");
                }
            }
        }
        return status_;
    }

  private:
    static void worker_entry(void* arg)
    {
        static_cast<SocketTcpTransport*>(arg)->worker();
    }

    [[noreturn]] void worker()
    {
        OpenRequest request{};
        for (;;)
        {
            if (xQueueReceive(queue_, &request, portMAX_DELAY) != pdTRUE)
            {
                continue;
            }
            const char* detail = "connect_failed";
            uint16_t error = TM_C6_ERROR_NOT_CONNECTED;
            const int fd = connect_endpoint(request, detail, error);
            std::lock_guard<std::mutex> lock(mutex_);
            if (request.generation != generation_)
            {
                if (fd >= 0)
                {
                    close(fd); // closed or opened again meanwhile
                }
                continue;
            }
            if (fd < 0)
            {
                ESP_LOGW(kTag, "connect %s:%u failed: %s", request.host, static_cast<unsigned>(request.port), detail);
                set_locked(WifiTcpState::Error, error, detail);
                continue;
            }
            fd_ = fd;
            set_locked(WifiTcpState::Connected, TM_C6_OK, "connected");
        }
    }

    bool ensure_worker()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_ != nullptr)
        {
            return true;
        }
        if (queue_ == nullptr)
        {
            queue_ = xQueueCreate(1, sizeof(OpenRequest));
            if (queue_ == nullptr)
            {
                return false;
            }
        }
        if (xTaskCreate(&SocketTcpTransport::worker_entry, "tcp_open", kWorkerStackBytes, this, kWorkerPriority,
                        &worker_) != pdPASS)
        {
            worker_ = nullptr;
            return false;
        }
        return true;
    }

    void set_locked(WifiTcpState state, uint16_t error, const char* detail) const
    {
        status_.state = state;
        status_.error_code = error;
        status_.detail = detail;
        if (state != WifiTcpState::Connected)
        {
            status_.buffered_bytes = 0;
        }
    }

    void close_locked() const
    {
        if (fd_ >= 0)
        {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
        status_.buffered_bytes = 0;
    }

    void refresh_buffered_locked() const
    {
        int pending = 0;
        status_.buffered_bytes = (fd_ >= 0 && ioctl(fd_, FIONREAD, &pending) == 0 && pending > 0)
                                     ? static_cast<size_t>(pending)
                                     : 0;
    }

    // tcpStatus() is const in the interface but notices a remote close, as the HostLink
    // transport's status does.
    mutable std::mutex mutex_;
    mutable WifiTcpStatus status_{};
    mutable int fd_ = -1;
    uint32_t generation_ = 0;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t worker_ = nullptr;
};

SocketTcpTransport s_socket_tcp_transport;

} // namespace

WifiTcpTransport& socket_tcp_transport()
{
    return s_socket_tcp_transport;
}

} // namespace platform::esp::idf_common::wireless_companion
