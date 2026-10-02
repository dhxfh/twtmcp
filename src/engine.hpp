#pragma once
// ============================================================================
// engine.hpp —— 内存搜索引擎（基于 TwtDriver）
// ============================================================================
// 提供 GG 风格的内存操作：
//   - 精确搜索（byte/word/dword/qword/float/double）
//   - 模糊搜索（未知初值 + increased/decreased/unchanged/changed 细化）
//   - 区间搜索（min~max）
//   - 结果集读写改
//   - 冻结（后台线程持续写回）
//   - 偏移计算 / 指针扫描
//   - 暂停/恢复进程
// ============================================================================

#include "twt_driver.hpp"

#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class ValType { Byte, Word, Dword, Qword, Float, Double };

inline int type_size(ValType t)
{
    switch (t)
    {
    case ValType::Byte:   return 1;
    case ValType::Word:   return 2;
    case ValType::Dword:  return 4;
    case ValType::Qword:  return 8;
    case ValType::Float:  return 4;
    case ValType::Double: return 8;
    }
    return 4;
}

inline bool parse_type(const std::string &s, ValType &out)
{
    if (s == "byte"   || s == "b")  { out = ValType::Byte;   return true; }
    if (s == "word"   || s == "w")  { out = ValType::Word;   return true; }
    if (s == "dword"  || s == "d" || s == "int")  { out = ValType::Dword;  return true; }
    if (s == "qword"  || s == "q" || s == "long") { out = ValType::Qword;  return true; }
    if (s == "float"  || s == "f")  { out = ValType::Float;  return true; }
    if (s == "double" || s == "dd") { out = ValType::Double; return true; }
    return false;
}

// 把用户字符串按类型编码成字节
// 注意：不用 std::stoll/std::stof —— NDK 静态链 libc++ 下可能因 locale/异常
// 配置抛异常被吞掉，导致「参数编码失败」。改用 C 的 strto* 系列，稳。
inline bool encode_value(ValType t, const std::string &text, std::vector<uint8_t> &out)
{
    out.resize(type_size(t));
    if (text.empty())
        return false;

    char *endp = nullptr;
    errno = 0;
    const char *s = text.c_str();

    switch (t)
    {
    case ValType::Byte:
    {
        long v = strtol(s, &endp, 0);
        if (endp == s) return false;
        int8_t x = (int8_t)v;
        memcpy(out.data(), &x, 1);
        return true;
    }
    case ValType::Word:
    {
        long v = strtol(s, &endp, 0);
        if (endp == s) return false;
        int16_t x = (int16_t)v;
        memcpy(out.data(), &x, 2);
        return true;
    }
    case ValType::Dword:
    {
        long long v = strtoll(s, &endp, 0);
        if (endp == s) return false;
        int32_t x = (int32_t)v;
        memcpy(out.data(), &x, 4);
        return true;
    }
    case ValType::Qword:
    {
        long long v = strtoll(s, &endp, 0);
        if (endp == s) return false;
        memcpy(out.data(), &v, 8);
        return true;
    }
    case ValType::Float:
    {
        float x = strtof(s, &endp);
        if (endp == s) return false;
        memcpy(out.data(), &x, 4);
        return true;
    }
    case ValType::Double:
    {
        double x = strtod(s, &endp);
        if (endp == s) return false;
        memcpy(out.data(), &x, 8);
        return true;
    }
    }
    return false;
}

inline std::string decode_value(ValType t, const uint8_t *p)
{
    char buf[64];
    switch (t)
    {
    case ValType::Byte:   { int8_t   x; memcpy(&x, p, 1); snprintf(buf, sizeof(buf), "%d", x); break; }
    case ValType::Word:   { int16_t  x; memcpy(&x, p, 2); snprintf(buf, sizeof(buf), "%d", x); break; }
    case ValType::Dword:  { int32_t  x; memcpy(&x, p, 4); snprintf(buf, sizeof(buf), "%d", x); break; }
    case ValType::Qword:  { int64_t  x; memcpy(&x, p, 8); snprintf(buf, sizeof(buf), "%lld", (long long)x); break; }
    case ValType::Float:  { float    x; memcpy(&x, p, 4); snprintf(buf, sizeof(buf), "%.7g", x); break; }
    case ValType::Double: { double   x; memcpy(&x, p, 8); snprintf(buf, sizeof(buf), "%.15g", x); break; }
    }
    return buf;
}

struct Result
{
    uintptr_t addr;
    std::string value;
    std::string prev; // 模糊搜索：上一次的值
};

struct SearchRange
{
    uintptr_t start = 0;
    uintptr_t end   = 0;
};

// /proc/<pid>/maps 里解析出的可搜索段
inline std::vector<SearchRange> parse_maps(pid_t pid, const std::string &filter)
{
    std::vector<SearchRange> out;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return out;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        // 只搜可读段
        if (strstr(line, " r") == nullptr)
            continue;

        uintptr_t s = 0, e = 0;
        if (sscanf(line, "%lx-%lx", (unsigned long *)&s, (unsigned long *)&e) != 2)
            continue;

        // filter: "anon"=匿名段(无路径) / "all"=全部 / 其它=路径子串
        bool has_path = strchr(line, '/') != nullptr;
        bool keep = false;
        if (filter.empty() || filter == "all")
            keep = true;
        else if (filter == "anon" || filter == "anonymous")
            keep = !has_path;
        else
            keep = strstr(line, filter.c_str()) != nullptr;

        if (keep && e > s)
            out.push_back({s, e});
    }
    fclose(f);
    return out;
}

class Engine
{
public:
    // scan_matrix 诊断统计（供调用方在 0 命中时报告）
    int      scan_stat_segs     = 0;
    uint64_t scan_stat_bytes    = 0;
    int      scan_stat_readfail = 0;

    struct FuzzyEntry
    {
        uintptr_t            addr;
        std::vector<uint8_t> prev;
    };

    struct FreezeEntry
    {
        uintptr_t            addr;
        ValType              type;
        std::string          text;
        std::vector<uint8_t> bytes;
        bool                 active;
    };

    struct MatrixHit
    {
        uintptr_t addr;
        float     m[16];
    };

    Engine(TwtDriver &drv) : drv_(drv) {}
    ~Engine() { unfreeze_all(); }

    bool attached() const { return drv_.target_pid() > 0; }
    pid_t target_pid() const { return drv_.target_pid(); }

    // ── 精确搜索 ─────────────────────────────────────────────────────────
    // region: ""/"all" 全内存, "anon" 匿名段, 或模块名子串
    int search_exact(ValType t, const std::string &text, const std::string &region,
                     std::vector<Result> &out, size_t max_results)
    {
        out.clear();
        std::vector<uint8_t> needle;
        if (!encode_value(t, text, needle))
            return -1;

        int sz = type_size(t);
        auto segs = parse_maps(drv_.target_pid(), region);

        const size_t BLK = 1 << 20; // 1MB 一块
        std::vector<uint8_t> buf(BLK);

        for (auto &seg : segs)
        {
            for (uintptr_t a = seg.start; a < seg.end; a += BLK)
            {
                size_t want = (size_t)std::min<uintptr_t>(BLK, seg.end - a);
                if (!drv_.read(a, buf.data(), want))
                    continue;

                for (size_t i = 0; i + sz <= want; ++i)
                {
                    if (memcmp(buf.data() + i, needle.data(), sz) == 0)
                    {
                        out.push_back({a + i, text, ""});
                        if (out.size() >= max_results)
                            return (int)out.size();
                    }
                }
            }
        }
        return (int)out.size();
    }

    // ── 区间搜索 ─────────────────────────────────────────────────────────
    // 对齐修正：float/dword 等按各自 size 对齐扫描，避免逐字节滑动造成
    // 的错位命中（小 float 位模式几乎全落在 -1~1 内，不对齐会命中海量垃圾）。
    // 同时跳过 0 与 |v|<1e-6 的噪声。
    int search_range(ValType t, const std::string &lo, const std::string &hi,
                     const std::string &region, std::vector<Result> &out, size_t max_results)
    {
        out.clear();
        std::vector<uint8_t> a, b;
        if (!encode_value(t, lo, a) || !encode_value(t, hi, b))
            return -1;

        auto segs = parse_maps(drv_.target_pid(), region);
        int sz = type_size(t);
        const size_t BLK = 1 << 20;
        std::vector<uint8_t> buf(BLK);

        double lo_d = 0, hi_d = 0;
        bool numeric = false;
        if (t == ValType::Float || t == ValType::Double)
        {
            lo_d = strtod(lo.c_str(), nullptr);
            hi_d = strtod(hi.c_str(), nullptr);
            numeric = true;
        }

        for (auto &seg : segs)
        {
            // 对齐到段起始的 sz 边界
            uintptr_t aligned = (seg.start + (sz - 1)) & ~((uintptr_t)sz - 1);
            for (uintptr_t p = aligned; p < seg.end; p += BLK)
            {
                size_t want = (size_t)std::min<uintptr_t>(BLK, seg.end - p);
                if (!drv_.read(p, buf.data(), want))
                    continue;

                // 按 sz 步进，不错位
                for (size_t i = 0; i + sz <= want; i += sz)
                {
                    bool hit = false;
                    if (numeric)
                    {
                        double v = (t == ValType::Float)
                                       ? (double)*(float *)(buf.data() + i)
                                       : *(double *)(buf.data() + i);
                        // 跳过 0 与极小噪声
                        if (v != 0.0 && !(v > -1e-6 && v < 1e-6))
                            hit = (v >= lo_d && v <= hi_d);
                    }
                    else
                    {
                        int64_t v = 0;
                        memcpy(&v, buf.data() + i, sz);
                        int64_t l = 0, h = 0;
                        memcpy(&l, a.data(), sz);
                        memcpy(&h, b.data(), sz);
                        if (v != 0)
                            hit = (v >= l && v <= h);
                    }
                    if (hit)
                    {
                        out.push_back({p + i, decode_value(t, buf.data() + i), ""});
                        if (out.size() >= max_results)
                            return (int)out.size();
                    }
                }
            }
        }
        return (int)out.size();
    }

    // ── 4×4 矩阵扫描（投影/视图矩阵专用）──────────────────────────────────
    // 在 region 内按 4 字节对齐滑动，找「16 个连续 float 组成的 4×4 矩阵」：
    //   - 每元素 |v| <= val_bound（默认 100）
    //   - 真实非零元素（|v|>eps）>= min_nonzero
    //   - 非零元素至少分布在 min_rows 行上（默认 2，排除稀疏噪声）
    //   mode: 0=不限  1=GL/列主序 第4行(12..15)≈0 0 0 1
    //         2=UE4/行主序第3行末列(索引11)≈1 且 索引15≈0
    // 返回候选矩阵的起始地址。
    int scan_matrix_once(const std::string &region, float val_bound, int min_nonzero,
                    int min_rows, int max_nonzero, int mode,
                    std::vector<MatrixHit> &out, size_t max_results,
                    const float *cam_pos = nullptr, float tol_ndc = 0.05f,
                    uintptr_t addr_start = 0, uintptr_t addr_end = 0)
    {
        out.clear();
        auto segs = parse_maps(drv_.target_pid(), region);
        // 诊断统计
        scan_stat_segs = (int)segs.size();
        scan_stat_bytes = 0;
        scan_stat_readfail = 0;
        const size_t BLK = 1 << 20;
        std::vector<uint8_t> buf(BLK + 64); // 多留一段给跨块矩阵
        const float EPS = 1e-6f;

        for (auto &seg : segs)
        {
            uintptr_t seg_lo = seg.start, seg_hi = seg.end;
            if (addr_start || addr_end)
            {
                if (addr_start > seg_lo) seg_lo = addr_start;
                if (addr_end   < seg_hi) seg_hi = addr_end;
                if (seg_lo >= seg_hi) continue;
            }
            uintptr_t aligned = (seg_lo + 3) & ~(uintptr_t)3;
            for (uintptr_t p = aligned; p < seg_hi; p += BLK)
            {
                size_t want = (size_t)std::min<uintptr_t>(BLK + 64, seg_hi - p);
                if (want < 64)
                    break;
                scan_stat_bytes += want;
                // 容错读：段内可能有未映射页/空洞，整块读会失败。
                // 策略：先整块读；失败则退化为按 4KB 页读，失败页填 0 后继续。
                // 这样段内空洞不会让整个段被丢弃（此前正是因此命中 0）。
                bool got = drv_.read(p, buf.data(), want);
                if (!got)
                {
                    ++scan_stat_readfail;
                    const size_t PAGE = 4096;
                    memset(buf.data(), 0, want);
                    for (size_t off = 0; off < want; off += PAGE)
                    {
                        size_t ps = std::min(PAGE, want - off);
                        drv_.read(p + off, buf.data() + off, ps);  // 失败保持 0
                    }
                }

                size_t lim = (want >= 64) ? (want - 64) : 0;
                for (size_t i = 0; i <= lim; i += 4) // 严格 4 字节对齐
                {
                    float *f = (float *)(buf.data() + i);
                    int   nonzero = 0, rows = 0;
                    bool  inb     = true;
                    for (int r = 0; r < 4; ++r)
                    {
                        int rn = 0;
                        for (int c = 0; c < 4; ++c)
                        {
                            float v = f[r * 4 + c];
                            if (v != v) { inb = false; break; }     // NaN
                            // 排除 denormal（0x00000001 之类被误读的整数位）
                            if (std::fpclassify(v) == FP_SUBNORMAL) { inb = false; break; }
                            // mode=4（视图矩阵）：第 4 行是相机位置，跳过范围限制
                            bool skip_bound = (mode == 4 && r == 3);
                            if (!skip_bound && (v > val_bound || v < -val_bound)) { inb = false; break; }
                            if (std::fabs(v) > EPS) { ++rn; ++nonzero; }
                        }
                        if (!inb) break;
                        if (rn > 0) ++rows;
                    }
                    if (!inb || nonzero < min_nonzero || rows < min_rows)
                        continue;
                    if (max_nonzero > 0 && nonzero > max_nonzero)
                        continue;

                    auto near0 = [](float v, float e = 1e-3f) { return std::fabs(v) < e; };

                    if (mode == 1)
                    {
                        // GL/列主序 透视投影：最后一列 (索引 3,7,11,15) = (0,0,-1,0)
                        // 且第 3 行 (索引 8,9) 前两列为 0
                        if (!(near0(f[3]) && near0(f[7]) && std::fabs(f[11] + 1.0f) < 1e-3f &&
                              near0(f[15]) && near0(f[8]) && near0(f[9])))
                            continue;
                    }
                    else if (mode == 2)
                    {
                        // UE4/行主序 透视投影：第 3 行 (索引 8,9,10,11) = (0,0,0,1)
                        // 第 4 行 (索引 12,13) 前两列为 0，索引 15 为 0，索引 14 非零
                        if (!(near0(f[8]) && near0(f[9]) && std::fabs(f[11] - 1.0f) < 1e-3f &&
                              near0(f[12]) && near0(f[13]) && near0(f[15]) &&
                              std::fabs(f[14]) > 1e-6f))
                            continue;
                    }
                    else if (mode == 3)
                    {
                        // GL/列主序 正交投影：最后一列 (索引 3,7,11,15) = (0,0,0,1)
                        if (!(near0(f[3]) && near0(f[7]) && near0(f[11]) &&
                              std::fabs(f[15] - 1.0f) < 1e-3f))
                            continue;
                    }
                    else if (mode == 4)
                    {
                        // 视图/相机矩阵：左上 3×3 是旋转（行主序），
                        // 索引 12,13,14 是相机位置（任意实数）。
                        auto dot3 = [&](int r1, int r2) {
                            float s = 0;
                            for (int c = 0; c < 3; ++c) s += f[r1 * 4 + c] * f[r2 * 4 + c];
                            return s;
                        };
                        auto len3 = [&](int r) {
                            float s = 0;
                            for (int c = 0; c < 3; ++c) s += f[r * 4 + c] * f[r * 4 + c];
                            return std::sqrt(s);
                        };
                        if (!(std::fabs(len3(0) - 1.0f) < 0.02f &&
                              std::fabs(len3(1) - 1.0f) < 0.02f &&
                              std::fabs(len3(2) - 1.0f) < 0.02f &&
                              std::fabs(dot3(0, 1)) < 0.02f &&
                              std::fabs(dot3(0, 2)) < 0.02f &&
                              std::fabs(dot3(1, 2)) < 0.02f))
                            continue;
                    }
                    else if (mode == 5 || mode == 6)
                    {
                        // 相机自洽（ViewProjection 反向定位）：
                        // 把相机自身世界位置 pos 投入该行主序矩阵，必须落在 NDC 中心。
                        //   w  = x*M12 + y*M13 + z*M14 + M15
                        //   cx = x*M0  + y*M1  + z*M2  + M3
                        //   cy = x*M4  + y*M5  + z*M6  + M7
                        // 硬约束（防近零噪声假阳性）：
                        //   · w 落在合理范围 [1e-3, 1e7]（相机在近平面外的深度量级）
                        //   · w 行必须有显著元素（|M12|+|M13|+|M14|+|M15| 够大），
                        //     否则相机坐标乘近零系数会得到无意义的 w
                        //   · NDC 中心容差 tol_ndc
                        //
                        // ⚠️ mode=5 的相机自洽判据对 View 和 VP **都成立**：
                        //    View 矩阵末行就是相机世界位置，代入自然落 NDC 中心；
                        //    VP 矩阵把相机位置投到中心也是必然。实测两者都命中。
                        // mode=6 = mode=5 + 强制 ViewProjection 判据（排除 View）：
                        //    View 的第 4 列 = (0,0,0,1) → f[12]=f[13]=f[14]=0，w 恒为 1；
                        //    VP  的第 4 列含投影尺度（如 -41927 这种大数），前三个不全零。
                        if (!cam_pos)
                            continue;
                        float X = cam_pos[0], Y = cam_pos[1], Z = cam_pos[2];
                        float wrow = std::fabs(f[12]) + std::fabs(f[13]) + std::fabs(f[14]) + std::fabs(f[15]);
                        if (wrow < 1e-2f)                      // w 行近乎全零 → 判据退化
                            continue;
                        if (mode == 6)
                        {
                            // 强制 ViewProjection：第 4 列前三个不能全零（View 恒为 0,0,0）
                            if (std::fabs(f[12]) + std::fabs(f[13]) + std::fabs(f[14]) < 1e-6f)
                                continue;
                        }
                        float w  = X * f[12] + Y * f[13] + Z * f[14] + f[15];
                        // ⚠️ w 的符号取决于投影方向（相机朝向 / NDC 约定），正负都合法。
                        //    实测：同一链在相机抬头时 w 为负（-1.07e9），要求 w>0 会误杀真 VP。
                        //    只要求 |w| 非近零即可。
                        if (!(std::fabs(w) > 1e-3f))
                            continue;
                        if (!std::isfinite(w))
                            continue;
                        float cx = X * f[0] + Y * f[1] + Z * f[2] + f[3];
                        float cy = X * f[4] + Y * f[5] + Z * f[6] + f[7];
                        float nx = cx / w, ny = cy / w;
                        if (!(std::isfinite(nx) && std::isfinite(ny)))
                            continue;
                        if (!(std::fabs(nx) <= tol_ndc && std::fabs(ny) <= tol_ndc))
                            continue;
                        // 平移行不能是「相机坐标 × 常数」那种纯缩放堆叠：
                        // 真 VP 矩阵平移行与相机位置量级相当，且不随坐标线性放大到天文数字
                        if (!(std::fabs(f[12]) < 1e8f && std::fabs(f[13]) < 1e8f &&
                              std::fabs(f[14]) < 1e8f && std::fabs(f[15]) < 1e8f))
                            continue;
                    }

                    MatrixHit h;
                    h.addr = p + i;
                    for (int k = 0; k < 16; ++k)
                        h.m[k] = f[k];
                    out.push_back(h);
                    if (out.size() >= max_results)
                        return (int)out.size();
                }
            }
        }
        return (int)out.size();
    }

    // ── 多帧投票包装 ─────────────────────────────────────────────────────
    // votes>1 时连扫 N 帧，只保留「在 >= support 帧里都命中」的地址。
    // 目的：真矩阵每帧都满足判据；跨块错位/瞬态噪声只会偶尔命中，被投票滤掉。
    // 每帧用同一套判据（含 mode=6 的 ViewProjection 强制判据）。
    // 注意：内存实时变化时（相机移动），真矩阵位置稳定但内容在变，投票只比地址。
    int scan_matrix(const std::string &region, float val_bound, int min_nonzero,
                    int min_rows, int max_nonzero, int mode,
                    std::vector<MatrixHit> &out, size_t max_results,
                    const float *cam_pos = nullptr, float tol_ndc = 0.05f,
                    uintptr_t addr_start = 0, uintptr_t addr_end = 0,
                    int votes = 1)
    {
        out.clear();
        if (votes <= 1)
            return scan_matrix_once(region, val_bound, min_nonzero, min_rows,
                                    max_nonzero, mode, out, max_results,
                                    cam_pos, tol_ndc, addr_start, addr_end);

        // 每帧的结果：地址 -> 命中时的矩阵内容
        std::vector<std::unordered_map<uintptr_t, MatrixHit>> frames;
        for (int f = 0; f < votes; ++f)
        {
            std::vector<MatrixHit> hit;
            scan_matrix_once(region, val_bound, min_nonzero, min_rows,
                             max_nonzero, mode, hit, max_results,
                             cam_pos, tol_ndc, addr_start, addr_end);
            std::unordered_map<uintptr_t, MatrixHit> m;
            for (auto &h : hit) m[h.addr] = h;
            frames.push_back(std::move(m));
        }

        // 判定：地址每帧都命中，且**各帧矩阵内容逐位一致**。
        // 冻结（freeze）时真矩阵内容静止 → 逐位一致；噪声块内容通常也在变/或只是偶尔命中。
        auto same_m = [](const float *a, const float *b) {
            for (int k = 0; k < 16; ++k)
                if (memcmp(&a[k], &b[k], sizeof(float)) != 0) return false;
            return true;
        };

        for (auto &kv : frames[0])
        {
            uintptr_t a = kv.first;
            const float *ref = kv.second.m;
            bool all = true;
            for (int f = 1; f < votes && all; ++f)
            {
                auto it = frames[f].find(a);
                if (it == frames[f].end()) { all = false; break; }
                if (!same_m(ref, it->second.m)) all = false;
            }
            if (!all) continue;
            out.push_back(kv.second);
            if (out.size() >= max_results) break;
        }
        return (int)out.size();
    }

    // ── 一键反查链 ───────────────────────────────────────────────────────
    // 从「矩阵对象地址」逐级反查谁指向它，直到追进模块（.so）段或达到上限级数。
    // region：反查指针时扫描的区域（默认 anon，矩阵对象指针通常在堆）。
    // max_level：最多反查几级（默认 3）。
    // 返回：[{obj, chain[]}]，chain 是「上级地址 + 该级偏移」序列。
    struct ChainLevel {
        uintptr_t holder;    // 存放「下一级地址」的字段的绝对地址
        uintptr_t value;     // holder 里存的值（= 指向的下一级地址）
        uintptr_t from_base; // holder - 模块基址（追到模块后才有意义）
        uintptr_t from_prev; // holder - 上一级 holder 的地址
    };
    struct ChainResult {
        uintptr_t               obj;       // 起点（矩阵对象）
        std::vector<ChainLevel> levels;    // levels[0] 最近，往上递增
        uintptr_t               mod_base;  // 追到模块段则为其基址，否则 0
        std::string             mod_name;
        bool                    reached_mod;
    };

    // 输入矩阵对象地址，逐级反查「谁指向它」，直至追进 .so 段。
    // 输出各级 holder 的绝对地址与相对模块基址的偏移。
    int find_chain(uintptr_t obj, std::vector<ChainResult> &out,
                   const std::string &region = "anon", int max_level = 4,
                   size_t max_holders = 8)
    {
        out.clear();
        ChainResult r;
        r.obj = obj;
        r.mod_base = 0;
        r.reached_mod = false;

        // 直接读 /proc/pid/maps，挑出 .so 模块段（parse_maps 不带 path）
        struct ModSeg { uintptr_t start, end; std::string path; };
        std::vector<ModSeg> mods;
        {
            char pth[64];
            snprintf(pth, sizeof(pth), "/proc/%d/maps", (int)drv_.target_pid());
            FILE *fp = fopen(pth, "r");
            if (fp)
            {
                char line[512];
                while (fgets(line, sizeof(line), fp))
                {
                    uintptr_t a, b;
                    char perms[8] = {0}, path[400] = {0};
                    if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %399s",
                               (unsigned long long *)&a, (unsigned long long *)&b,
                               perms, path) >= 3)
                    {
                        if (path[0] && strstr(path, ".so"))
                        {
                            ModSeg ms; ms.start = a; ms.end = b; ms.path = path;
                            mods.push_back(ms);
                        }
                    }
                }
                fclose(fp);
            }
        }
        auto in_mod = [&](uintptr_t a, uintptr_t &base, std::string &name) -> bool {
            for (auto &s : mods)
                if (s.start <= a && a < s.end)
                { base = s.start; name = s.path; return true; }
            return false;
        };

        uintptr_t cur = obj;
        uintptr_t prev_holder = 0;
        for (int lv = 0; lv < max_level; ++lv)
        {
            std::vector<uintptr_t> holders;
            pointer_scan(cur, region, holders, max_holders);
            if (holders.empty()) break;

            uintptr_t mb = 0; std::string mn;
            uintptr_t best = 0;
            for (uintptr_t h : holders)
                if (in_mod(h, mb, mn)) { best = h; break; }
            if (!best) best = holders[0];

            ChainLevel cl;
            cl.holder    = best;
            cl.value     = cur;
            cl.from_prev = prev_holder ? (best - prev_holder) : 0;

            if (in_mod(best, mb, mn))
            {
                cl.from_base = best - mb;
                r.mod_base = mb;
                r.mod_name = mn;
                r.reached_mod = true;
                r.levels.push_back(cl);
                break;
            }
            cl.from_base = 0;
            r.levels.push_back(cl);
            prev_holder = best;
            cur = best;
        }

        out.push_back(r);
        return 1;
    }

    int fuzzy_init(ValType t, const std::string &region, size_t max_results)
    {
        fuzzy_.clear();
        fuzzy_type_ = t;
        int sz = type_size(t);
        auto segs = parse_maps(drv_.target_pid(), region);

        const size_t BLK = 1 << 20;
        std::vector<uint8_t> buf(BLK);
        std::vector<uint8_t> snap; // 全部候选值快照

        for (auto &seg : segs)
        {
            for (uintptr_t a = seg.start; a < seg.end; a += BLK)
            {
                size_t want = (size_t)std::min<uintptr_t>(BLK, seg.end - a);
                if (!drv_.read(a, buf.data(), want))
                    continue;
                for (size_t i = 0; i + sz <= want; ++i)
                {
                    FuzzyEntry fe;
                    fe.addr = a + i;
                    fe.prev.assign(buf.data() + i, buf.data() + i + sz);
                    fuzzy_.push_back(std::move(fe));
                    if (fuzzy_.size() >= max_results)
                        return (int)fuzzy_.size();
                }
            }
        }
        return (int)fuzzy_.size();
    }

    // ── 模糊搜索：细化 ───────────────────────────────────────────────────
    // cond: increased / decreased / unchanged / changed / exact:<value>
    int fuzzy_refine(ValType t, const std::string &cond, const std::string &arg)
    {
        int sz = type_size(t);
        std::vector<uint8_t> cur(sz);
        std::vector<FuzzyEntry> keep;
        keep.reserve(fuzzy_.size() / 2 + 1);

        bool exact = cond.rfind("exact:", 0) == 0;
        std::vector<uint8_t> want;
        if (exact && !encode_value(t, cond.substr(6), want))
            return -1;

        for (auto &fe : fuzzy_)
        {
            if (!drv_.read(fe.addr, cur.data(), sz))
                continue;

            bool ok = false;
            if (exact)
            {
                ok = (memcmp(cur.data(), want.data(), sz) == 0);
            }
            else if (cond == "unchanged")
            {
                ok = (memcmp(cur.data(), fe.prev.data(), sz) == 0);
            }
            else if (cond == "changed")
            {
                ok = (memcmp(cur.data(), fe.prev.data(), sz) != 0);
            }
            else
            {
                // 数值比较（含 float/double）
                if (t == ValType::Float || t == ValType::Double)
                {
                    double c, p;
                    if (t == ValType::Float) { float x, y; memcpy(&x, cur.data(), 4); memcpy(&y, fe.prev.data(), 4); c = x; p = y; }
                    else { memcpy(&c, cur.data(), 8); memcpy(&p, fe.prev.data(), 8); }
                    if (cond == "increased") ok = c > p;
                    else if (cond == "decreased") ok = c < p;
                }
                else
                {
                    int64_t c = 0, p = 0;
                    memcpy(&c, cur.data(), sz);
                    memcpy(&p, fe.prev.data(), sz);
                    if (cond == "increased") ok = c > p;
                    else if (cond == "decreased") ok = c < p;
                }
            }

            if (ok)
            {
                fe.prev.assign(cur.begin(), cur.end());
                keep.push_back(std::move(fe));
            }
        }
        fuzzy_ = std::move(keep);
        return (int)fuzzy_.size();
    }

    const std::vector<FuzzyEntry> &fuzzy_entries() const { return fuzzy_; }

    // ── 结果集读写 ───────────────────────────────────────────────────────
    bool read_at(uintptr_t addr, ValType t, std::string &out)
    {
        std::vector<uint8_t> b(type_size(t));
        if (!drv_.read(addr, b.data(), b.size()))
            return false;
        out = decode_value(t, b.data());
        return true;
    }

    bool write_at(uintptr_t addr, ValType t, const std::string &text)
    {
        std::vector<uint8_t> b;
        if (!encode_value(t, text, b))
            return false;
        return drv_.write(addr, b.data(), b.size());
    }

    // ── 冻结 ─────────────────────────────────────────────────────────────
    bool freeze(uintptr_t addr, ValType t, const std::string &text)
    {
        std::vector<uint8_t> b;
        if (!encode_value(t, text, b))
            return false;

        std::lock_guard<std::mutex> lk(fz_mtx_);
        auto it = freezes_.find(addr);
        if (it != freezes_.end())
        {
            it->second.bytes = b;
            return true;
        }
        FreezeEntry fe;
        fe.addr   = addr;
        fe.type   = t;
        fe.text   = text;
        fe.bytes  = b;
        fe.active = true;
        freezes_[addr] = std::move(fe);
        start_freeze_thread();
        return true;
    }

    bool unfreeze(uintptr_t addr)
    {
        std::lock_guard<std::mutex> lk(fz_mtx_);
        return freezes_.erase(addr) > 0;
    }

    void unfreeze_all()
    {
        {
            std::lock_guard<std::mutex> lk(fz_mtx_);
            freezes_.clear();
        }
        stop_freeze_thread();
    }

    size_t freeze_count()
    {
        std::lock_guard<std::mutex> lk(fz_mtx_);
        return freezes_.size();
    }

    std::vector<uintptr_t> freeze_list()
    {
        std::vector<uintptr_t> v;
        std::lock_guard<std::mutex> lk(fz_mtx_);
        for (auto &kv : freezes_)
            v.push_back(kv.first);
        return v;
    }

    // ── 指针扫描（一层）───────────────────────────────────────────────────
    // 在 region 里找「值等于 target 的地址」的区域，作为指向 target 的指针候选
    int pointer_scan(uintptr_t target, const std::string &region,
                     std::vector<uintptr_t> &out, size_t max_results)
    {
        out.clear();
        auto segs = parse_maps(drv_.target_pid(), region);
        const size_t BLK = 1 << 20;
        std::vector<uint8_t> buf(BLK);
        std::vector<uint8_t> needle(8);
        memcpy(needle.data(), &target, 8);

        for (auto &seg : segs)
        {
            for (uintptr_t a = seg.start; a < seg.end; a += BLK)
            {
                size_t want = (size_t)std::min<uintptr_t>(BLK, seg.end - a);
                if (!drv_.read(a, buf.data(), want))
                    continue;
                for (size_t i = 0; i + 8 <= want; i += 8) // 指针按 8 字节对齐扫
                {
                    if (memcmp(buf.data() + i, needle.data(), 8) == 0)
                    {
                        out.push_back(a + i);
                        if (out.size() >= max_results)
                            return (int)out.size();
                    }
                }
            }
        }
        return (int)out.size();
    }

private:
    TwtDriver &drv_;
    ValType    fuzzy_type_ = ValType::Dword;
    std::vector<FuzzyEntry> fuzzy_;

    std::mutex                        fz_mtx_;
    std::unordered_map<uintptr_t, FreezeEntry> freezes_;
    std::thread                       fz_thread_;
    std::atomic<bool>                 fz_run_{false};

    void start_freeze_thread()
    {
        if (fz_run_)
            return;
        fz_run_ = true;
        fz_thread_ = std::thread([this]() {
            while (fz_run_)
            {
                {
                    std::lock_guard<std::mutex> lk(fz_mtx_);
                    for (auto &kv : freezes_)
                    {
                        auto &f = kv.second;
                        if (f.active)
                            drv_.write(f.addr, f.bytes.data(), f.bytes.size());
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    void stop_freeze_thread()
    {
        fz_run_ = false;
        if (fz_thread_.joinable())
            fz_thread_.join();
    }
};
