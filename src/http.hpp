#pragma once
// ============================================================================
// http.hpp —— 极简 HTTP/1.1 服务器（阻塞式，够 MCP 用）
// ============================================================================
// 支持: GET/POST、Content-Length、keep-alive 关闭（每请求一连接）、并发线程。
// 监听地址可配 0.0.0.0（局域网）或 127.0.0.1。
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct HttpReq
{
    std::string method;
    std::string path;
    std::string body;
    std::string remote_ip;
    std::string headers_raw;                 // 原始 header 文本（小写化后）
    std::map<std::string, std::string> headers; // 解析后的头

    std::string header(const std::string &k) const
    {
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResp
{
    int         status = 200;
    std::string type   = "application/json";
    std::string body;

    static HttpResp json(const std::string &b, int code = 200)
    {
        HttpResp r;
        r.status = code;
        r.type   = "application/json";
        r.body   = b;
        return r;
    }
    static HttpResp text(const std::string &b, int code = 200)
    {
        HttpResp r;
        r.status = code;
        r.type   = "text/plain; charset=utf-8";
        r.body   = b;
        return r;
    }
};

using HttpHandler = std::function<HttpResp(const HttpReq &)>;

class HttpServer
{
public:
    HttpServer(const std::string &bind_ip, int port) : ip_(bind_ip), port_(port) {}
    ~HttpServer() { stop(); }

    void set_handler(HttpHandler h) { handler_ = std::move(h); }

    bool start()
    {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
            return false;

        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr = {};
        addr.sin_family  = AF_INET;
        addr.sin_port    = htons((uint16_t)port_);
        if (ip_.empty() || ip_ == "0.0.0.0")
            addr.sin_addr.s_addr = INADDR_ANY;
        else
            inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

        if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
        {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (listen(listen_fd_, 32) < 0)
        {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        running_ = true;
        accept_thread_ = std::thread([this]() { accept_loop(); });
        return true;
    }

    void stop()
    {
        if (!running_)
            return;
        running_ = false;
        if (listen_fd_ >= 0)
        {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable())
            accept_thread_.join();
    }

    int port() const { return port_; }

private:
    std::string ip_;
    int         port_;
    int         listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    HttpHandler handler_;

    void accept_loop()
    {
        while (running_)
        {
            sockaddr_in cli = {};
            socklen_t   len = sizeof(cli);
            int c = accept(listen_fd_, (sockaddr *)&cli, &len);
            if (c < 0)
            {
                if (!running_)
                    break;
                continue;
            }
            std::string rip = inet_ntoa(cli.sin_addr);
            std::thread([this, c, rip]() {
                handle_conn(c, rip);
                close(c);
            }).detach();
        }
    }

    static bool recv_all(int fd, std::string &out, size_t want)
    {
        out.clear();
        char buf[4096];
        while (out.size() < want)
        {
            ssize_t n = recv(fd, buf, std::min(sizeof(buf), want - out.size()), 0);
            if (n <= 0)
                return false;
            out.append(buf, n);
        }
        return true;
    }

    void handle_conn(int fd, const std::string &rip)
    {
        // 读到 header 结束
        std::string data;
        char        buf[4096];
        size_t      header_end = std::string::npos;
        while (data.size() < 64 * 1024)
        {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            data.append(buf, n);
            header_end = data.find("\r\n\r\n");
            if (header_end != std::string::npos)
                break;
        }
        if (header_end == std::string::npos)
            return;

        std::string head = data.substr(0, header_end);
        std::string body = data.substr(header_end + 4);

        // 解析请求行
        HttpReq req;
        req.remote_ip = rip;
        {
            size_t sp1 = head.find(' ');
            size_t sp2 = head.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos)
                return;
            req.method = head.substr(0, sp1);
            req.path   = head.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        // Content-Length
        size_t cl = 0;
        {
            std::string lower = head;
            for (auto &c : lower)
                c = (char)tolower((unsigned char)c);
            size_t p = lower.find("content-length:");
            if (p != std::string::npos)
                cl = (size_t)strtoul(head.c_str() + p + 15, nullptr, 10);
        }

        // 补齐 body
        while (body.size() < cl)
        {
            ssize_t n = recv(fd, buf, std::min(sizeof(buf), cl - body.size()), 0);
            if (n <= 0)
                break;
            body.append(buf, n);
        }
        req.body = body;

        // 解析 header（小写化 key）
        req.headers_raw = head;
        {
            size_t pos = head.find("\r\n");
            if (pos == std::string::npos)
                pos = 0;
            else
                pos += 2;
            while (pos < head.size())
            {
                size_t eol = head.find("\r\n", pos);
                if (eol == std::string::npos)
                    eol = head.size();
                std::string line = head.substr(pos, eol - pos);
                size_t      c    = line.find(':');
                if (c != std::string::npos)
                {
                    std::string k = line.substr(0, c);
                    std::string v = line.substr(c + 1);
                    // trim
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                        v.erase(v.begin());
                    for (auto &ch : k)
                        ch = (char)tolower((unsigned char)ch);
                    req.headers[k] = v;
                }
                pos = eol + 2;
            }
        }

        HttpResp resp;
        if (handler_)
        {
            try
            {
                resp = handler_(req);
            }
            catch (...)
            {
                resp = HttpResp::json("{\"error\":\"internal\"}", 500);
            }
        }
        else
            resp = HttpResp::text("no handler", 500);

        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n";
        out += "Content-Type: " + resp.type + "\r\n";
        out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        out += "Access-Control-Allow-Origin: *\r\n";
        out += "\r\n";
        out += resp.body;

        send(fd, out.data(), out.size(), 0);
    }

    static const char *status_text(int s)
    {
        switch (s)
        {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        }
        return "OK";
    }
};
