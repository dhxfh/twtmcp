#pragma once
// ============================================================================
// lan.hpp —— 局域网能力：UDP 广播设备发现 + token 鉴权 + 设备列表
// ============================================================================
// 参考 agg（局域网控制器）的做法：
//  - 服务端在 UDP 端口上监听 "TWT_DISCOVER"，回发自己的能力/端点信息
//  - 客户端可广播发现，收集局域网内所有 twtmcp 节点
//  - HTTP 侧用共享 token 鉴权（Header: X-TWT-Token 或 ?token=）
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

struct LanInfo
{
    std::string name;       // 设备名（默认 hostname）
    std::string version;    // 版本
    int         http_port;  // MCP HTTP 端口
    int         disc_port;  // 发现端口
    pid_t       target_pid; // 当前 attach 的 pid（0=未 attach）
};

class LanDiscovery
{
public:
    LanDiscovery() = default;
    ~LanDiscovery() { stop(); }

    // info_fn: 每次收到发现请求时回调，返回当前节点信息
    using InfoFn = std::function<LanInfo()>;

    bool start(InfoFn fn, int disc_port = 8789)
    {
        fn_        = std::move(fn);
        disc_port_ = disc_port;

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0)
            return false;

        int one = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

        sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)disc_port_);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock_, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            close(sock_);
            sock_ = -1;
            return false;
        }

        running_ = true;
        thr_ = std::thread([this]() { loop(); });
        return true;
    }

    void stop()
    {
        running_ = false;
        if (sock_ >= 0)
        {
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
            sock_ = -1;
        }
        if (thr_.joinable())
            thr_.join();
    }

private:
    int         sock_ = -1;
    int         disc_port_ = 8789;
    std::atomic<bool> running_{false};
    std::thread thr_;
    InfoFn      fn_;

    void loop()
    {
        char buf[512];
        while (running_)
        {
            sockaddr_in from = {};
            socklen_t   flen = sizeof(from);
            ssize_t n = recvfrom(sock_, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &flen);
            if (n <= 0)
            {
                if (!running_)
                    break;
                continue;
            }
            buf[n] = '\0';

            // 只认约定的发现报文
            if (strncmp(buf, "TWT_DISCOVER", 12) != 0)
                continue;

            LanInfo info = fn_ ? fn_() : LanInfo{};
            std::string reply = "TWT_HERE|" + info.name + "|" + info.version + "|" +
                                std::to_string(info.http_port) + "|" +
                                std::to_string(info.disc_port) + "|" +
                                std::to_string(info.target_pid);
            sendto(sock_, reply.data(), reply.size(), 0, (sockaddr *)&from, flen);
        }
    }
};

// ── 客户端：广播发现，收集节点 ────────────────────────────────────────────
struct DiscoveredNode
{
    std::string ip;
    std::string name;
    std::string version;
    int         http_port = 0;
    int         disc_port = 0;
    pid_t       target_pid = 0;
};

inline std::vector<DiscoveredNode> lan_discover(int disc_port = 8789, int wait_ms = 1200)
{
    std::vector<DiscoveredNode> out;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return out;

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in b = {};
    b.sin_family      = AF_INET;
    b.sin_port        = htons((uint16_t)disc_port);
    b.sin_addr.s_addr = INADDR_BROADCAST;

    const char *msg = "TWT_DISCOVER";
    sendto(s, msg, strlen(msg), 0, (sockaddr *)&b, sizeof(b));

    // 限时收集应答
    timeval tv{};
    tv.tv_sec  = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[512];
    while (true)
    {
        sockaddr_in from = {};
        socklen_t   flen = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &flen);
        if (n <= 0)
            break;
        buf[n] = '\0';

        if (strncmp(buf, "TWT_HERE|", 9) != 0)
            continue;

        // TWT_HERE|name|ver|http|disc|pid
        std::string                  s2(buf + 9);
        std::vector<std::string>     parts;
        size_t                       p = 0, q;
        while ((q = s2.find('|', p)) != std::string::npos)
        {
            parts.push_back(s2.substr(p, q - p));
            p = q + 1;
        }
        parts.push_back(s2.substr(p));
        if (parts.size() < 5)
            continue;

        DiscoveredNode node;
        node.ip         = inet_ntoa(from.sin_addr);
        node.name       = parts[0];
        node.version    = parts[1];
        node.http_port  = atoi(parts[2].c_str());
        node.disc_port  = atoi(parts[3].c_str());
        node.target_pid = (pid_t)atoi(parts[4].c_str());
        out.push_back(node);
    }
    close(s);
    return out;
}
