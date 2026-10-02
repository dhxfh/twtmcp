#pragma once
// ============================================================================
// twt_driver.hpp —— TwT 驱动后端（headless 版）
// ============================================================================
// 从 twt/kernel.h 的 c_driver 提取，去掉交互式 gyro/touch 初始化提示，
// 改为纯 API 形式，供 MCP server 无终端环境使用。
//
// 协议（对齐 twt/kernel.h）：
//   装 fd : MY_CALL(0x114514, 0x1919810, 0x2778, &fd)
//   识别  : /proc/self/fd -> anon_inode:TwT_driver
//   读    : ioctl(fd, READ_MEM  = _IOW('T',4, request), &{pid,addr,buf,size})
//   写    : ioctl(fd, WRITE_MEM = _IOW('T',5, request), &{pid,addr,buf,size})
//   其他  : GET_PID(0) / MODULE_BASE(1) / MODULE_BSS(3) / READ_MEM_V2(11)
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <limits.h>

#define TWT_MARK        'T'
#define TWT_DRIVER_NAME "TwT_driver"

#define TWT_GET_PID      _IOW(TWT_MARK, 0,  struct twt_request)
#define TWT_MODULE_BASE  _IOW(TWT_MARK, 1,  struct twt_request)
#define TWT_MODULE_BSS   _IOW(TWT_MARK, 3,  struct twt_request)
#define TWT_READ_MEM     _IOW(TWT_MARK, 4,  struct twt_request)
#define TWT_WRITE_MEM    _IOW(TWT_MARK, 5,  struct twt_request)
#define TWT_READ_MEM_V2  _IOW(TWT_MARK, 11, struct twt_request)

// 单次 ioctl 尺寸上限（TwT 内核侧有解包缓冲限制，超过分块）
// ⚠️ 实测：单次 ioctl 读 >4KB 会失败（返回非 0），4096 稳定成功。
//    原来定 64KB 导致扫描时整块读取失败（命中 0）。改 4096。
#ifndef TWT_IO_CHUNK
#define TWT_IO_CHUNK (4096)
#endif

struct twt_request
{
    pid_t     pid;
    uintptr_t addr;
    void     *buffer;
    size_t    size;
};

// 手写寄存器直呼 svc #0（对应 kernel.h 的 MY_CALL 宏）
static inline long twt_my_call(long magic1, long magic2, long cmd, long arg)
{
    long ret;
    register long _x0 __asm__("x0") = magic1;
    register long _x1 __asm__("x1") = magic2;
    register long _x2 __asm__("x2") = cmd;
    register long _x3 __asm__("x3") = arg;
    register long _nr __asm__("x8") = __NR_reboot;
    __asm__ __volatile__(
        "svc #0"
        : "=r"(_x0)
        : "r"(_x0), "r"(_x1), "r"(_x2), "r"(_x3), "r"(_nr)
        : "memory", "cc");
    ret = _x0;
    return ret;
}

class TwtDriver
{
public:
    struct ModuleInfo
    {
        std::string name;
        uintptr_t   base;
        uintptr_t   bss;
    };

    TwtDriver() { open_driver(); }

    ~TwtDriver()
    {
        if (fd_ >= 0)
            close(fd_);
    }

    bool ready() const { return fd_ >= 0; }
    int  fd()    const { return fd_; }

    pid_t target_pid() const { return pid_; }
    bool  attach(pid_t p)
    {
        if (p <= 0)
            return false;
        pid_ = p;
        return true;
    }

    // ── 按进程名取 pid ───────────────────────────────────────────────────
    pid_t pid_of(const std::string &name)
    {
        if (fd_ < 0 || name.empty())
            return -1;

        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name.c_str());

        twt_request req = {};
        req.pid    = 0;
        req.buffer = buf;

        if (ioctl(fd_, TWT_GET_PID, &req) != 0)
            return -1;
        return req.pid;
    }

    // 内核 GET_PID 失败时的兜底：扫 /proc/<pid>/cmdline
    static pid_t scan_pid_by_name(const std::string &name)
    {
        DIR *d = opendir("/proc");
        if (!d)
            return -1;

        pid_t found = -1;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr)
        {
            int p = atoi(e->d_name);
            if (p <= 0)
                continue;

            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/cmdline", p);

            int f = ::open(path, O_RDONLY);
            if (f < 0)
                continue;

            char cl[512] = {};
            ssize_t n = ::read(f, cl, sizeof(cl) - 1);
            ::close(f);
            if (n <= 0)
                continue;

            // cmdline 用 \0 分隔，取第一段做子串匹配
            if (strstr(cl, name.c_str()) != nullptr)
            {
                found = p;
                break;
            }
        }
        closedir(d);
        return found;
    }

    // ── 读 ───────────────────────────────────────────────────────────────
    bool read(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd_ < 0 || pid_ <= 0 || buffer == nullptr || size == 0)
            return false;

        addr &= 0xFFFFFFFFFFFFULL;

        uint8_t *dst  = (uint8_t *)buffer;
        size_t   done = 0;
        while (done < size)
        {
            size_t chunk = size - done;
            if (chunk > TWT_IO_CHUNK)
                chunk = TWT_IO_CHUNK;

            twt_request req = {};
            req.pid    = pid_;
            req.addr   = addr + done;
            req.buffer = dst + done;
            req.size   = chunk;

            if (ioctl(fd_, TWT_READ_MEM, &req) != 0)
                return false;
            done += chunk;
        }
        return true;
    }

    bool read_v2(uintptr_t addr, void *buffer, size_t size)
    {
        if (fd_ < 0 || pid_ <= 0 || buffer == nullptr || size == 0)
            return false;

        addr &= 0xFFFFFFFFFFFFULL;
        uint8_t *dst  = (uint8_t *)buffer;
        size_t   done = 0;
        while (done < size)
        {
            size_t chunk = size - done;
            if (chunk > TWT_IO_CHUNK)
                chunk = TWT_IO_CHUNK;

            twt_request req = {};
            req.pid    = pid_;
            req.addr   = addr + done;
            req.buffer = dst + done;
            req.size   = chunk;

            if (ioctl(fd_, TWT_READ_MEM_V2, &req) != 0)
                return false;
            done += chunk;
        }
        return true;
    }

    template <typename T>
    T read_t(uintptr_t addr)
    {
        T v{};
        read(addr, &v, sizeof(T));
        return v;
    }

    // ── 写 ───────────────────────────────────────────────────────────────
    bool write(uintptr_t addr, const void *buffer, size_t size)
    {
        if (fd_ < 0 || pid_ <= 0 || buffer == nullptr || size == 0)
            return false;

        addr &= 0xFFFFFFFFFFFFULL;
        const uint8_t *src  = (const uint8_t *)buffer;
        size_t         done = 0;
        while (done < size)
        {
            size_t chunk = size - done;
            if (chunk > TWT_IO_CHUNK)
                chunk = TWT_IO_CHUNK;

            twt_request req = {};
            req.pid    = pid_;
            req.addr   = addr + done;
            req.buffer = (void *)(src + done);
            req.size   = chunk;

            if (ioctl(fd_, TWT_WRITE_MEM, &req) != 0)
                return false;
            done += chunk;
        }
        return true;
    }

    // ── 模块基址 / BSS ───────────────────────────────────────────────────
    uintptr_t module_base(const std::string &name)
    {
        if (fd_ < 0 || pid_ <= 0 || name.empty())
            return 0;

        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name.c_str());

        twt_request req = {};
        req.pid    = pid_;
        req.addr   = 0;
        req.buffer = buf;

        if (ioctl(fd_, TWT_MODULE_BASE, &req) != 0)
            return 0;
        return req.addr;
    }

    uintptr_t module_bss(const std::string &name)
    {
        if (fd_ < 0 || pid_ <= 0 || name.empty())
            return 0;

        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name.c_str());

        twt_request req = {};
        req.pid    = pid_;
        req.addr   = 0;
        req.buffer = buf;

        if (ioctl(fd_, TWT_MODULE_BSS, &req) != 0)
            return 0;
        return req.addr;
    }

    // ── 读 /proc/<pid>/maps，取所有模块（用于 tool：module_list）─────────
    std::vector<ModuleInfo> module_list()
    {
        std::vector<ModuleInfo> out;
        if (pid_ <= 0)
            return out;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/maps", pid_);
        FILE *f = fopen(path, "r");
        if (!f)
            return out;

        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            // 只看可执行段，取路径里的文件名做模块名
            if (strstr(line, "r-xp") == nullptr)
                continue;
            char *slash = strrchr(line, '/');
            if (!slash)
                continue;
            char *nl = strchr(slash, '\n');
            if (nl)
                *nl = '\0';

            std::string full = slash;
            std::string nm   = full.substr(full.find_last_of('/') + 1);
            if (nm.empty())
                continue;

            uintptr_t base = 0;
            sscanf(line, "%lx", (unsigned long *)&base);

            bool dup = false;
            for (auto &m : out)
                if (m.name == nm)
                {
                    dup = true;
                    break;
                }
            if (!dup)
                out.push_back({nm, base, 0});
        }
        fclose(f);
        return out;
    }

private:
    int   fd_  = -1;
    pid_t pid_ = -1;

    void open_driver()
    {
        fd_ = -1;
        // 1) 内核 Magic 装 fd
        twt_my_call(0x114514, 0x1919810, 0x2778, (long)&fd_);
        // 2) 扫 /proc/self/fd 兜底
        if (fd_ < 0)
            fd_ = scan_fd();
        if (fd_ < 0)
            fprintf(stderr, "[twt] 驱动对接失败：找不到 %s\n", TWT_DRIVER_NAME);
        else
            fprintf(stderr, "[twt] 驱动对接成功 fd=%d\n", fd_);
    }

    int scan_fd()
    {
        DIR *dir = opendir("/proc/self/fd");
        if (!dir)
            return -1;

        int found = -1;
        struct dirent *e;
        char link[PATH_MAX], target[PATH_MAX];
        while ((e = readdir(dir)) != nullptr)
        {
            if (e->d_name[0] == '.')
                continue;
            snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n < 0)
                continue;
            target[n] = '\0';
            if (strstr(target, TWT_DRIVER_NAME) != nullptr)
            {
                found = atoi(e->d_name);
                break;
            }
        }
        closedir(dir);
        return found;
    }
};
