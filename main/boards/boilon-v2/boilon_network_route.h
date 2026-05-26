#ifndef BOILON_NETWORK_ROUTE_H_
#define BOILON_NETWORK_ROUTE_H_

#include <string>
#include <esp_log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "wifi_manager.h"

namespace boilon_network_route {

constexpr const char* kExternalHost = "175.178.247.160";
constexpr const char* kInternalHost = "192.168.1.170";
constexpr int kInternalOtaPort = 18002;
constexpr int kInternalProbeTimeoutMs = 800;

inline bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

inline void ReplaceAll(std::string& value, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.length(), to);
        pos += to.length();
    }
}

inline bool ProbeTcp(const char* host, int port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock);
        return false;
    }

    int ret = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        close(sock);
        return true;
    }

    if (errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeval tv = {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
    if (ret <= 0) {
        close(sock);
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
        error = errno;
    }
    close(sock);
    return error == 0;
}

inline bool IsInternalNetworkAvailable(const char* tag) {
    static bool checked = false;
    static bool available = false;
    if (checked) {
        return available;
    }
    checked = true;

    std::string local_ip = WifiManager::GetInstance().GetIpAddress();
    if (!StartsWith(local_ip, "192.168.1.")) {
        ESP_LOGI(tag, "Skip internal probe: local IP is %s", local_ip.c_str());
        return false;
    }

    ESP_LOGI(tag, "Probing internal TCP endpoint: %s:%d", kInternalHost, kInternalOtaPort);
    available = ProbeTcp(kInternalHost, kInternalOtaPort, kInternalProbeTimeoutMs);
    ESP_LOGI(tag, "Internal TCP probe available=%s", available ? "true" : "false");
    return available;
}

inline std::string RewriteHostForActiveNetwork(std::string url, const char* tag) {
    if (url.empty()) {
        return url;
    }

    if (IsInternalNetworkAvailable(tag)) {
        ReplaceAll(url, kExternalHost, kInternalHost);
    } else {
        ReplaceAll(url, kInternalHost, kExternalHost);
    }
    return url;
}

inline std::string RewriteStrokeUrlForActiveNetwork(std::string url, const char* tag) {
    if (IsInternalNetworkAvailable(tag)) {
        const std::string external = "http://175.178.247.160:18090/strokes";
        const std::string internal = "http://192.168.1.170:8090/strokes";
        if (StartsWith(url, external)) {
            url.replace(0, external.length(), internal);
            ESP_LOGI(tag, "Rewrite stroke URL to internal: %s", url.c_str());
        }
    }
    return url;
}

}  // namespace boilon_network_route

#endif  // BOILON_NETWORK_ROUTE_H_
