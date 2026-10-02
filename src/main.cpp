// ============================================================================
// main.cpp —— twtmcp：TwT 驱动 + MCP Server（局域网可访问）
// ============================================================================
// 把 TwT 内核驱动的内存读写能力，通过 MCP（Model Context Protocol）暴露给
// AI 客户端。参考 gameguardian-mcp-server（Go）的 26 个工具设计，用 C++ 复刻，
// 驱动后端换成 TwT（twt/kernel.h 协议）。
//
// 传输：HTTP JSON-RPC（POST /mcp），默认监听 0.0.0.0:8788（局域网可访问）
// 鉴权：X-TWT-Token 头 或 ?token=xxx（配置了 --token 才启用）
// 发现：UDP 8789 广播（TWT_DISCOVER / TWT_HERE）
//
// 用法：
//   twtmcp                          # 0.0.0.0:8788，无鉴权
//   twtmcp -l 127.0.0.1:8788        # 仅本机
//   twtmcp --token mysecret         # 局域网 + token 鉴权
//   twtmcp --discover               # 只做一次局域网扫描，列出节点后退出
// ============================================================================

#include "json.hpp"
#include "twt_driver.hpp"
#include "engine.hpp"
#include "http.hpp"
#include "lan.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

static const char *kVersion = "1.0.0";

// ── 全局状态 ─────────────────────────────────────────────────────────────
struct App
{
    TwtDriver  drv;
    Engine     eng{drv};
    ValType    last_type = ValType::Dword;
    std::string last_region;
    std::mutex  mtx; // 工具调用串行化（驱动 fd 共享）
};

// ── 工具定义（26 个，对齐 gameguardian-mcp-server）─────────────────────────
struct ToolDef
{
    const char *name;
    const char *desc;
};

static const ToolDef kTools[] = {
    {"gg_list_processes",     "列出所有进程（pid/名称），可按关键字过滤"},
    {"gg_attach",             "附加目标进程（pid 或包名）,初始化内核通道"},
    {"gg_detach",             "分离当前进程"},
    {"gg_target_info",        "目标进程内存映射概况 + 模块列表"},
    {"gg_module_list",        "已加载模块（文件名+基址）"},
    {"gg_module_base",        "取指定模块基址"},
    {"gg_search",             "已知值精确搜索（byte/word/dword/qword/float/double）"},
    {"gg_search_range",       "区间搜索（min~max）"},
    {"gg_search_unknown_init","模糊搜索初始化（未知初值，记录快照）"},
    {"gg_search_refine",      "模糊细化：increased/decreased/unchanged/changed/exact:V"},
    {"gg_result_count",       "结果数量"},
    {"gg_get_results",        "查看结果（地址+当前值）"},
    {"gg_clear_results",      "清空结果"},
    {"gg_read",               "读指定地址的值"},
    {"gg_read_bytes",         "读指定地址的原始字节（hex）"},
    {"gg_hex_dump",           "hex 转储"},
    {"gg_write",              "写指定地址"},
    {"gg_set_results",        "批量修改搜索结果（按 index 或 address）"},
    {"gg_freeze",             "冻结值（后台持续写回）"},
    {"gg_unfreeze",           "解冻"},
    {"gg_freeze_list",        "列出所有冻结项"},
    {"gg_calc_offset",        "地址相对模块基址的偏移"},
    {"gg_pointer_scan",       "指针扫描（找指向目标地址的指针）"},
    {"gg_pause",              "暂停进程（SIGSTOP）"},
    {"gg_resume",             "恢复进程（SIGCONT）"},
    {"gg_scan_matrix",        "扫描 4×4 float 矩阵（投影/视图矩阵），按结构特征过滤"},
    {"gg_find_chain",         "一键反查链：从矩阵对象地址逐级反查指针，追到模块基址得完整多级偏移链"},
    {"gg_view_matrix",        "读 DFM 视图投影矩阵（ViewProjection）：[[[libUE4.so+0x202EE508]+0xCC0]+0xB28] 读 16 float，行主序，含自洽校验"},
    {"gg_camera",             "读本地玩家相机（位置/旋转/FOV）：UWorld+0x190→+0x38→+0x0→+0x30→+0x408，+0x37C0 起 0x20 字节"},
    {"gg_maps",               "列出目标进程可搜索内存段（诊断用）"},
    {"gg_status",             "会话状态"},
};

// 工具 JSON Schema（只列关键字段，够 AI 用）
static mj::Value tool_schema(const std::string &name)
{
    mj::Value props = mj::Value::make_obj();
    auto add = [&](const char *k, const char *desc, const char *type = "string") {
        mj::Value p = mj::Value::make_obj();
        p.set("type", type);
        p.set("description", desc);
        props.set(k, p);
    };

    if (name == "gg_attach")            { add("package", "包名或进程名"); add("pid", "进程 pid", "number"); }
    else if (name == "gg_search")       { add("value", "要搜索的值"); add("type", "byte/word/dword/qword/float/double（默认 dword）"); add("region", "all/anon/模块名（默认 anon）"); add("max", "最大结果数", "number"); }
    else if (name == "gg_search_range") { add("min", "下界"); add("max", "上界"); add("type", "值类型"); add("region", "区域"); add("limit", "最大返回条数（默认100000）", "number"); }
    else if (name == "gg_search_unknown_init") { add("type", "值类型"); add("region", "区域"); add("max", "最大候选数", "number"); }
    else if (name == "gg_search_refine") { add("cond", "increased/decreased/unchanged/changed/exact:值"); add("type", "值类型（默认继承上次）"); }
    else if (name == "gg_get_results")  { add("offset", "起始下标", "number"); add("limit", "返回条数", "number"); }
    else if (name == "gg_read" || name == "gg_write") { add("address", "十六进制地址，如 0x7f1234 或纯hex"); add("type", "值类型"); if (name == "gg_write") add("value", "要写入的值"); }
    else if (name == "gg_read_bytes" || name == "gg_hex_dump") { add("address", "地址"); add("size", "字节数", "number"); }
    else if (name == "gg_module_base" || name == "gg_calc_offset") { add("module", "模块名，如 libUE4.so"); if (name == "gg_calc_offset") add("address", "目标地址"); }
    else if (name == "gg_set_results")  { add("values", "数组：[{index,value}] 或 [{address,value}]", "array"); add("type", "值类型"); }
    else if (name == "gg_freeze" || name == "gg_unfreeze") { add("address", "地址"); if (name == "gg_freeze") { add("value", "冻结值"); add("type", "值类型"); } }
    else if (name == "gg_pointer_scan") { add("target", "目标地址"); add("region", "区域"); add("max", "最大结果", "number"); }
    else if (name == "gg_find_chain") { add("address", "矩阵对象地址（十六进制）。可先用 gg_scan_matrix mode=6+freeze 扫出候选，再逐个反查"); add("region", "反查指针的扫描区域（默认 anon）"); add("levels", "最多反查级数（默认 4）", "number"); add("max_holders", "每级最多取几个候选指针（默认 8）", "number"); }
    else if (name == "gg_scan_matrix")   { add("region", "区域（默认 anon，矩阵常在 [anon:libc_malloc]/[heap]，可传 all）"); add("val_bound", "元素绝对值上限（默认 100）", "number"); add("min_nonzero", "真实非零元素最少个数（默认 8）", "number"); add("min_rows", "非零元素至少分布行数（默认 2）", "number"); add("max_nonzero", "真实非零元素最多个数（0=不限，用于排除全密纹理块）", "number"); add("mode", "0=不限 1=GL列主序透视 2=UE4行主序透视 3=GL列主序正交 4=视图/相机矩阵(左上3×3正交旋转) 5=相机自洽(相机位置投入须落屏幕中心，需 cam_x/y/z 或自动读) 6=相机自洽+强制ViewProjection(排除View，推荐)", "number"); add("row4_unit", "兼容旧参数，true 等价 mode=1", "boolean"); add("cam_x", "相机世界 X（mode=5 可选，不给则自动读）", "number"); add("cam_y", "相机世界 Y", "number"); add("cam_z", "相机世界 Z", "number"); add("tol_ndc", "mode=5 NDC 容差（默认 0.05）", "number"); add("addr_start", "可选：限定扫描起始地址（十六进制，含）"); add("addr_end", "可选：限定扫描结束地址（十六进制，不含）"); add("votes", "多帧投票：连扫 N 帧取地址交集，N>1 滤掉瞬态噪声（默认 1）；真矩阵每帧都中，噪声偶尔中", "number"); add("freeze", "扫描前 SIGSTOP 暂停目标、扫完 SIGCONT 恢复；配合 votes 能稳定命中内容实时变化的真矩阵（默认 false）", "boolean"); add("limit", "最大返回矩阵数（默认 200）", "number"); }
    else if (name == "gg_view_matrix") { add("module", "模块名（默认 libUE4.so）"); add("base_offset", "GMatrix2 相对模块基址的偏移（默认 0x202EE508）"); add("chain1", "第二级偏移（默认 0xCC0）"); add("chain2", "末级偏移（默认 0xB28，直接就是矩阵地址，不再解引用）"); add("verify", "是否做自洽投影校验（默认 true）", "boolean"); }
    else if (name == "gg_camera") { add("module", "模块名（默认 libUE4.so）"); }
    else if (name == "gg_list_processes") { add("filter", "名称过滤关键字"); }
    else if (name == "gg_pause" || name == "gg_resume") { add("pid", "pid（默认当前目标）", "number"); }

    mj::Value schema = mj::Value::make_obj();
    schema.set("type", "object");
    schema.set("properties", props);
    return schema;
}

// ── 工具实现 ─────────────────────────────────────────────────────────────

static std::string hex_of(uintptr_t v)
{
    char b[32];
    snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    return b;
}

static uintptr_t parse_addr(const mj::Value &args)
{
    mj::Value a = args.get("address");
    if (a.type == mj::Type::Num)
        return (uintptr_t)a.as_num();
    std::string s = a.as_str();
    if (s.empty())
        return 0;
    return (uintptr_t)strtoull(s.c_str(), nullptr, 0); // 支持 0x 前缀与纯 hex
}

// 结果集（GG 的搜索结果）
static std::vector<Result> g_results;

static mj::Value ok_text(const std::string &t)
{
    mj::Value r = mj::Value::make_obj();
    mj::Value c = mj::Value::make_arr();
    mj::Value item = mj::Value::make_obj();
    item.set("type", "text");
    item.set("text", t);
    c.push(item);
    r.set("content", c);
    return r;
}

static mj::Value err_text(const std::string &t)
{
    mj::Value r = ok_text(t);
    r.set("isError", true);
    return r;
}

static mj::Value call_tool(App &app, const std::string &name, const mj::Value &args)
{
    TwtDriver &d = app.drv;

    // ── 进程管理 ──
    if (name == "gg_list_processes")
    {
        std::string filter = args.get("filter").as_str();
        std::string out;
        FILE *f = popen("ps -A -o pid,comm 2>/dev/null || ps -A 2>/dev/null", "r");
        if (!f)
            return err_text("无法读取进程列表");
        char line[512];
        int  cnt = 0;
        while (fgets(line, sizeof(line), f))
        {
            if (!filter.empty() && !strstr(line, filter.c_str()))
                continue;
            out += line;
            if (++cnt > 500)
                break;
        }
        pclose(f);
        return ok_text(out.empty() ? "(无匹配进程)" : out);
    }

    if (name == "gg_attach")
    {
        pid_t p = 0;
        if (args.has("pid") && args.get("pid").as_int() > 0)
            p = (pid_t)args.get("pid").as_int();
        else
        {
            std::string pkg = args.get("package").as_str();
            if (pkg.empty())
                return err_text("需要 package 或 pid");
            p = d.pid_of(pkg);
            if (p <= 0)
                p = TwtDriver::scan_pid_by_name(pkg);
        }
        if (p <= 0)
            return err_text("找不到目标进程");
        if (!d.attach(p))
            return err_text("attach 失败");
        g_results.clear();
        return ok_text("已附加 pid=" + std::to_string(p));
    }

    if (name == "gg_detach")
    {
        d.attach(-1);
        g_results.clear();
        return ok_text("已分离");
    }

    if (name == "gg_maps")
    {
        std::string region = args.get("region").as_str("anon");
        auto segs = parse_maps(d.target_pid(), region);
        unsigned long long total = 0;
        for (auto &s : segs)
            total += (s.end - s.start);
        std::string out = "区段数=" + std::to_string(segs.size()) +
                          "  总字节=" + std::to_string(total) + " (" +
                          std::to_string(total / 1024 / 1024) + " MB)\n";
        size_t show = segs.size() < 15 ? segs.size() : 15;
        for (size_t i = 0; i < show; ++i)
            out += hex_of(segs[i].start) + " - " + hex_of(segs[i].end) + "\n";
        // 探针：读第一个段的头 16 字节，验证大块读是否可用
        if (!segs.empty())
        {
            uint8_t probe[16];
            if (d.read(segs[0].start, probe, sizeof(probe)))
            {
                char b[64];
                snprintf(b, sizeof(b), "探针[%s] 读OK: %02x %02x %02x %02x\n",
                         hex_of(segs[0].start).c_str(), probe[0], probe[1], probe[2], probe[3]);
                out += b;
            }
            else
                out += "探针读失败\n";
        }
        return ok_text(out);
    }

    if (name == "gg_status")
    {
        std::string s = "驱动: " + std::string(d.ready() ? "已连接" : "未连接") + "\n";
        s += "目标 pid: " + std::to_string(d.target_pid()) + "\n";
        s += "结果数: " + std::to_string(g_results.size()) + "\n";
        s += "冻结项: " + std::to_string(app.eng.freeze_count()) + "\n";
        s += "默认类型/区域: " + std::to_string((int)app.last_type) + " / " + app.last_region;
        return ok_text(s);
    }

    if (name == "gg_target_info" || name == "gg_module_list")
    {
        if (!app.eng.attached())
            return err_text("未附加进程");
        auto mods = d.module_list();
        std::string s;
        for (auto &m : mods)
            s += m.name + "  base=" + hex_of(m.base) + "\n";
        return ok_text(s.empty() ? "(无模块)" : s);
    }

    if (name == "gg_module_base")
    {
        std::string mod = args.get("module").as_str();
        uintptr_t   b   = d.module_base(mod);
        if (!b)
            return err_text("取模块基址失败: " + mod);
        return ok_text(hex_of(b));
    }

    if (name == "gg_calc_offset")
    {
        std::string mod = args.get("module").as_str();
        uintptr_t   b   = d.module_base(mod);
        uintptr_t   a   = parse_addr(args);
        if (!b)
            return err_text("模块基址失败");
        long long off = (long long)a - (long long)b;
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)off);
        return ok_text(std::string("offset = ") + buf);
    }

    // ── 搜索 ──
    if (!app.eng.attached())
        return err_text("未附加进程（先 gg_attach）");

    if (name == "gg_search")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        std::string region = args.get("region").as_str("anon");
        size_t maxr = args.has("max") ? (size_t)args.get("max").as_int() : 100000;
        int n = app.eng.search_exact(t, args.get("value").as_str(), region, g_results, maxr);
        if (n < 0)
            return err_text("值编码失败");
        app.last_type = t;
        app.last_region = region;
        return ok_text("找到 " + std::to_string(n) + " 处");
    }

    if (name == "gg_search_range")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        size_t lim = args.has("limit") ? (size_t)args.get("limit").as_int() : 100000;
        int n = app.eng.search_range(t, args.get("min").as_str(), args.get("max").as_str(),
                                     args.get("region").as_str("anon"), g_results, lim);
        if (n < 0)
            return err_text("参数编码失败（min/max 必须是字符串形式的值）");
        app.last_type = t;
        return ok_text("区间找到 " + std::to_string(n) + " 处");
    }

    if (name == "gg_search_unknown_init")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        size_t maxr = args.has("max") ? (size_t)args.get("max").as_int() : 200000;
        int n = app.eng.fuzzy_init(t, args.get("region").as_str("anon"), maxr);
        app.last_type = t;
        app.last_region = args.get("region").as_str("anon");
        return ok_text("模糊初始化候选 " + std::to_string(n) + " 个");
    }

    if (name == "gg_search_refine")
    {
        int n = app.eng.fuzzy_refine(app.last_type, args.get("cond").as_str(), "");
        return ok_text("细化后剩 " + std::to_string(n) + " 个");
    }

    if (name == "gg_scan_matrix")
    {
        std::string region = args.get("region").as_str("anon");
        float  val_bound   = args.has("val_bound") ? (float)args.get("val_bound").as_num() : 100.0f;
        int    min_nonzero = args.has("min_nonzero") ? (int)args.get("min_nonzero").as_int() : 8;
        int    min_rows    = args.has("min_rows") ? (int)args.get("min_rows").as_int() : 2;
        int    max_nonzero = args.has("max_nonzero") ? (int)args.get("max_nonzero").as_int() : 0;
        int    mode        = args.has("mode") ? (int)args.get("mode").as_int() : 0;
        if (args.has("row4_unit") && args.get("row4_unit").as_bool())
            mode = 1; // 兼容旧参数
        size_t lim         = args.has("limit") ? (size_t)args.get("limit").as_int() : 200;

        // mode=5/6（相机自洽扫描）：需要相机世界位置。可从参数给，也可自动读。
        float  cam_pos[3] = {0, 0, 0};
        bool   have_cam   = false;
        if (mode == 5 || mode == 6)
        {
            if (args.has("cam_x") && args.has("cam_y") && args.has("cam_z"))
            {
                cam_pos[0] = (float)args.get("cam_x").as_num();
                cam_pos[1] = (float)args.get("cam_y").as_num();
                cam_pos[2] = (float)args.get("cam_z").as_num();
                have_cam = true;
            }
            else
            {
                // 自动读相机（同 gg_camera 的链）
                uintptr_t mb = d.module_base("libUE4.so");
                auto rdu = [&](uintptr_t a) -> uintptr_t { uintptr_t v = 0; return d.read(a, &v, sizeof(v)) ? v : 0; };
                auto vp  = [](uintptr_t p) { return p >= 0x10000000ULL && p < 0x10000000000ULL; };
                uintptr_t uw = mb ? rdu(mb + 0x205F5B50) : 0;
                uintptr_t pc = uw;
                const uintptr_t 链[] = {0x190, 0x38, 0x0, 0x30};
                for (uintptr_t off : 链) { if (!vp(pc)) break; pc = rdu(pc + off); }
                uintptr_t pcm = vp(pc) ? rdu(pc + 0x408) : 0;
                if (vp(pcm) && d.read(pcm + 0x37C0, cam_pos, sizeof(cam_pos)))
                    have_cam = true;
            }
            if (!have_cam)
                return err_text("mode=5/6 需要相机位置：请传 cam_x/cam_y/cam_z，或确认游戏进对局且已附加");
        }
        float tol_ndc = args.has("tol_ndc") ? (float)args.get("tol_ndc").as_num() : 0.05f;
        uintptr_t a_start = 0, a_end = 0;
        if (args.has("addr_start")) a_start = (uintptr_t)strtoull(args.get("addr_start").as_str().c_str(), nullptr, 0);
        if (args.has("addr_end"))   a_end   = (uintptr_t)strtoull(args.get("addr_end").as_str().c_str(), nullptr, 0);

        int votes = args.has("votes") ? (int)args.get("votes").as_int() : 1;
        if (votes < 1) votes = 1;
        if (votes > 64) votes = 64;

        // freeze：扫描前 SIGSTOP 暂停目标进程，扫完 SIGCONT 恢复。
        // 目的：真矩阵内容随相机实时变化，非冻结时各帧不一致，导致多帧投票误杀真矩阵。
        // 冻结后内存静止，真矩阵每帧稳定命中，投票才能真正滤掉噪声。
        bool freeze = args.has("freeze") && args.get("freeze").as_bool();
        pid_t tpid = app.eng.target_pid();
        bool frozen = false;
        if (freeze && tpid > 0)
        {
            if (kill(tpid, SIGSTOP) == 0) frozen = true;
        }

        std::vector<Engine::MatrixHit> hits;
        int n = app.eng.scan_matrix(region, val_bound, min_nonzero, min_rows, max_nonzero, mode, hits, lim,
                                    have_cam ? cam_pos : nullptr, tol_ndc, a_start, a_end, votes);

        if (frozen)
            kill(tpid, SIGCONT);   // 无论扫描成功与否都恢复，避免把游戏卡住
        if (n <= 0)
        {
            char db[200];
            snprintf(db, sizeof(db),
                     "未找到符合条件的 4×4 矩阵。\n诊断: 扫描段数=%d 总字节=%llu MB=%.1f 读失败块=%d\n"
                     "（可放宽 val_bound / 降 min_nonzero / region 传 all）",
                     app.eng.scan_stat_segs, (unsigned long long)app.eng.scan_stat_bytes,
                     app.eng.scan_stat_bytes / 1048576.0, app.eng.scan_stat_readfail);
            return ok_text(db);
        }

        std::string s = "找到 " + std::to_string(n) + " 个 4×4 矩阵候选";
        if (votes > 1) s += "（" + std::to_string(votes) + " 帧投票）";
        s += "：\n";
        if (mode == 5 || mode == 6)
        {
            char cb[160];
            snprintf(cb, sizeof(cb), "相机位置 = (%.3f, %.3f, %.3f)  tol_ndc=%.4f%s\n",
                     cam_pos[0], cam_pos[1], cam_pos[2], tol_ndc,
                     mode == 6 ? "  [强制 ViewProjection]" : "");
            s += cb;
        }
        size_t show = hits.size() < 30 ? hits.size() : 30;
        for (size_t i = 0; i < show; ++i)
        {
            s += "[" + std::to_string(i) + "] " + hex_of(hits[i].addr) + "\n";
            for (int r = 0; r < 4; ++r)
            {
                char line[160];
                snprintf(line, sizeof(line), "    %12.5f %12.5f %12.5f %12.5f\n",
                         hits[i].m[r * 4 + 0], hits[i].m[r * 4 + 1],
                         hits[i].m[r * 4 + 2], hits[i].m[r * 4 + 3]);
                s += line;
            }
        }
        if (hits.size() > show)
            s += "... 其余 " + std::to_string(hits.size() - show) + " 个省略\n";
        return ok_text(s);
    }

    // ── 相机（本地玩家：位置/旋转/FOV）──
    //   UWorld = *(libUE4 + 0x205F5B50)
    //   本地控制器 = UWorld+0x190 -> +0x38 -> +0x0 -> +0x30
    //   玩家相机   = 本地控制器 + 0x408
    //   相机块     = 玩家相机 + 0x37C0 起 0x20 字节（位置12/旋转12/FOV4）
    if (name == "gg_camera")
    {
        std::string mod = args.get("module").as_str("libUE4.so");
        uintptr_t   b   = d.module_base(mod);
        if (!b)
            return err_text("取模块基址失败: " + mod);

        auto rdu = [&](uintptr_t a) -> uintptr_t {
            uintptr_t v = 0;
            if (!d.read(a, &v, sizeof(v))) return 0;
            return v;
        };
        auto validp = [](uintptr_t p) { return p >= 0x10000000ULL && p < 0x10000000000ULL; };

        uintptr_t uw = rdu(b + 0x205F5B50);
        if (!validp(uw)) return err_text("UWorld 无效（游戏未进对局？）");
        uintptr_t pc = uw;
        const uintptr_t 链[] = {0x190, 0x38, 0x0, 0x30};
        for (uintptr_t off : 链) { if (!validp(pc)) break; pc = rdu(pc + off); }
        if (!validp(pc)) return err_text("本地控制器无效");

        uintptr_t pcm = rdu(pc + 0x408);
        if (!validp(pcm)) return err_text("玩家相机无效");

        uint8_t blk[0x20] = {0};
        if (!d.read(pcm + 0x37C0, blk, sizeof(blk)))
            return err_text("读相机块失败");
        float px, py, pz, pitch, yaw, roll, fov;
        memcpy(&px, blk + 0x00, 4); memcpy(&py, blk + 0x04, 4); memcpy(&pz, blk + 0x08, 4);
        memcpy(&pitch, blk + 0x10, 4); memcpy(&yaw, blk + 0x14, 4); memcpy(&roll, blk + 0x18, 4);
        memcpy(&fov, blk + 0x1C, 4);
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "UWorld          = %s\n本地控制器(PC)  = %s\n玩家相机(PCM)   = %s\n"
                 "位置 pos  = (%.3f, %.3f, %.3f)\n旋转 rot  = Pitch=%.3f Yaw=%.3f Roll=%.3f\nFOV       = %.3f\n",
                 hex_of(uw).c_str(), hex_of(pc).c_str(), hex_of(pcm).c_str(),
                 px, py, pz, pitch, yaw, roll, fov);
        return ok_text(buf);
    }

    // ── DFM 视图投影矩阵（ViewProjection）──
    //   链: [[[libUE4 + 0x202EE508] + 0xCC0] + 0xB28] 读 16 float（行主序）
    //   末段 +0xB28 直接就是矩阵地址，不再解引用。
    //   自洽校验：用 (gg_camera 的相机位置) 做 行主序投影，应落在屏幕中心。
    if (name == "gg_view_matrix")
    {
        std::string mod     = args.get("module").as_str("libUE4.so");
        uintptr_t   base_off = 0x202EE508ULL;
        uintptr_t   c1       = 0xCC0ULL;
        uintptr_t   c2       = 0xB28ULL;
        bool        verify   = !args.has("verify") || args.get("verify").as_bool();
        if (args.has("base_offset")) base_off = (uintptr_t)strtoull(args.get("base_offset").as_str().c_str(), nullptr, 0);
        if (args.has("chain1"))      c1       = (uintptr_t)strtoull(args.get("chain1").as_str().c_str(), nullptr, 0);
        if (args.has("chain2"))      c2       = (uintptr_t)strtoull(args.get("chain2").as_str().c_str(), nullptr, 0);

        uintptr_t b = d.module_base(mod);
        if (!b) return err_text("取模块基址失败: " + mod);

        auto rdu = [&](uintptr_t a) -> uintptr_t {
            uintptr_t v = 0;
            if (!d.read(a, &v, sizeof(v))) return 0;
            return v;
        };
        auto validp = [](uintptr_t p) { return p >= 0x10000000ULL && p < 0x10000000000ULL; };

        uintptr_t a1 = b + base_off;
        uintptr_t g2 = rdu(a1);
        if (!validp(g2)) return err_text("第一级指针无效: " + hex_of(a1));
        uintptr_t mid = rdu(g2 + c1);
        if (!validp(mid)) return err_text("第二级指针无效: " + hex_of(g2 + c1));
        uintptr_t mat = mid + c2;

        float m[16] = {0};
        if (!d.read(mat, m, sizeof(m)))
            return err_text("读矩阵失败: " + hex_of(mat));

        // 内容有效性（照搬 矩阵校验.h）
        int nz = 0;
        bool fin = true;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1.0e8f) { fin = false; break; }
            if (std::fabs(m[i]) > 1.0e-6f) nz++;
        }

        std::string s;
        char off1[32], off2[32];
        snprintf(off1, sizeof(off1), "0x%llx", (unsigned long long)c1);
        snprintf(off2, sizeof(off2), "0x%llx", (unsigned long long)c2);
        s += "链: [[" + hex_of(a1) + "] -> " + hex_of(g2) + "] +" + off1 +
             " -> " + hex_of(mid) + "] +" + off2 + " = " + hex_of(mat) + "\n";
        s += "布局: 行主序(row-major)  ViewProjection\n";
        s += "内容有效: " + std::string(fin ? "是" : "否(含 NaN/Inf/极值)") + "  非零数=" + std::to_string(nz) + "\n";
        for (int r = 0; r < 4; ++r) {
            char line[160];
            snprintf(line, sizeof(line), "  %14.6f %14.6f %14.6f %14.6f\n",
                     m[r*4+0], m[r*4+1], m[r*4+2], m[r*4+3]);
            s += line;
        }

        if (verify)
        {
            // 读相机位置，做行主序投影，验证落在屏幕中心
            uintptr_t uw = rdu(b + 0x205F5B50);
            uintptr_t pc = uw;
            const uintptr_t 链[] = {0x190, 0x38, 0x0, 0x30};
            for (uintptr_t off : 链) { if (!validp(pc)) break; pc = rdu(pc + off); }
            uintptr_t pcm = validp(pc) ? rdu(pc + 0x408) : 0;
            if (validp(pcm))
            {
                float cam[3] = {0};
                if (d.read(pcm + 0x37C0, cam, sizeof(cam)))
                {
                    // 行主序投影: cx = x*M0+y*M1+z*M2+M3 ; cy = x*M4+... ; w = x*M12+...M15
                    float X = cam[0], Y = cam[1], Z = cam[2];
                    float w  = X*m[12] + Y*m[13] + Z*m[14] + m[15];
                    float cx = X*m[0]  + Y*m[1]  + Z*m[2]  + m[3];
                    float cy = X*m[4]  + Y*m[5]  + Z*m[6]  + m[7];
                    char vb[256];
                    if (std::fabs(w) > 1e-9f) {
                        // 用绝对像素：NDC*半屏 + 中心。默认 2400x1080。
                        float ndcx = cx / w, ndcy = cy / w;
                        snprintf(vb, sizeof(vb),
                                 "自洽校验(相机自身位置应投到屏幕中心):\n"
                                 "  pos=(%.3f, %.3f, %.3f)\n  w=%.6f  ndc=(%.6f, %.6f)\n"
                                 "  → 2400x1080 下像素=(%.2f, %.2f)  期望(1200, 540)\n",
                                 cam[0], cam[1], cam[2], w, ndcx, ndcy,
                                 1200.0f + ndcx * 1200.0f, 540.0f - ndcy * 540.0f);
                    } else {
                        snprintf(vb, sizeof(vb), "自洽校验: w≈0，投影退化（矩阵可能不是 VP）\n");
                    }
                    s += vb;
                }
            }
            else s += "自洽校验: 跳过（相机链无效）\n";
        }
        return ok_text(s);
    }

    if (name == "gg_result_count")
        return ok_text(std::to_string(g_results.size()));

    if (name == "gg_clear_results")
    {
        g_results.clear();
        return ok_text("已清空");
    }

    if (name == "gg_get_results")
    {
        size_t off = args.has("offset") ? (size_t)args.get("offset").as_int() : 0;
        size_t lim = args.has("limit") ? (size_t)args.get("limit").as_int() : 50;
        std::string s;
        for (size_t i = off; i < g_results.size() && i < off + lim; ++i)
        {
            // 读当前值
            std::string cur;
            app.eng.read_at(g_results[i].addr, app.last_type, cur);
            s += "[" + std::to_string(i) + "] " + hex_of(g_results[i].addr) + " = " + cur + "\n";
        }
        return ok_text(s.empty() ? "(无结果)" : s);
    }

    // ── 读写 ──
    if (name == "gg_read")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        std::string v;
        if (!app.eng.read_at(parse_addr(args), t, v))
            return err_text("读失败");
        return ok_text(v);
    }

    if (name == "gg_read_bytes" || name == "gg_hex_dump")
    {
        uintptr_t a = parse_addr(args);
        size_t    n = args.has("size") ? (size_t)args.get("size").as_int() : 64;
        if (n > 4096)
            n = 4096;
        std::vector<uint8_t> b(n);
        if (!d.read(a, b.data(), n))
            return err_text("读失败");
        std::string s;
        char line[128];
        for (size_t i = 0; i < n; i += 16)
        {
            snprintf(line, sizeof(line), "%s: ", hex_of(a + i).c_str());
            s += line;
            for (size_t j = i; j < i + 16 && j < n; ++j)
            {
                snprintf(line, sizeof(line), "%02x ", b[j]);
                s += line;
            }
            s += "\n";
        }
        return ok_text(s);
    }

    if (name == "gg_write")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        if (!app.eng.write_at(parse_addr(args), t, args.get("value").as_str()))
            return err_text("写失败");
        return ok_text("已写入");
    }

    if (name == "gg_set_results")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        const mj::Value &arr = args.get("values");
        if (!arr.is_arr())
            return err_text("values 必须是数组");
        int ok_cnt = 0;
        for (size_t i = 0; i < arr.size(); ++i)
        {
            const mj::Value &e = arr.at(i);
            uintptr_t addr = 0;
            if (e.has("address"))
                addr = (uintptr_t)strtoull(e.get("address").as_str().c_str(), nullptr, 0);
            else if (e.has("index"))
            {
                size_t idx = (size_t)e.get("index").as_int();
                if (idx < g_results.size())
                    addr = g_results[idx].addr;
            }
            if (addr && app.eng.write_at(addr, t, e.get("value").as_str()))
                ++ok_cnt;
        }
        return ok_text("已修改 " + std::to_string(ok_cnt) + " 处");
    }

    // ── 冻结 ──
    if (name == "gg_freeze")
    {
        ValType t;
        if (!parse_type(args.get("type").as_str("dword"), t))
            return err_text("未知类型");
        if (!app.eng.freeze(parse_addr(args), t, args.get("value").as_str()))
            return err_text("冻结失败");
        return ok_text("已冻结");
    }

    if (name == "gg_unfreeze")
    {
        if (!app.eng.unfreeze(parse_addr(args)))
            return err_text("该地址未冻结");
        return ok_text("已解冻");
    }

    if (name == "gg_freeze_list")
    {
        auto v = app.eng.freeze_list();
        std::string s;
        for (auto a : v)
            s += hex_of(a) + "\n";
        return ok_text(s.empty() ? "(无冻结项)" : s);
    }

    // ── 指针扫描 ──
    if (name == "gg_pointer_scan")
    {
        uintptr_t target = (uintptr_t)strtoull(args.get("target").as_str().c_str(), nullptr, 0);
        size_t    maxr   = args.has("max") ? (size_t)args.get("max").as_int() : 200;
        std::vector<uintptr_t> hits;
        int n = app.eng.pointer_scan(target, args.get("region").as_str("anon"), hits, maxr);
        std::string s;
        for (auto a : hits)
            s += hex_of(a) + "\n";
        return ok_text("指针候选 " + std::to_string(n) + " 个\n" + s);
    }

    // ── 一键反查链 ──
    if (name == "gg_find_chain")
    {
        uintptr_t obj = (uintptr_t)strtoull(args.get("address").as_str().c_str(), nullptr, 0);
        if (!app.eng.attached())
            return err_text("未附加进程（先 gg_attach）");
        int levels = args.has("levels") ? (int)args.get("levels").as_int() : 4;
        size_t mh  = args.has("max_holders") ? (size_t)args.get("max_holders").as_int() : 8;
        std::vector<Engine::ChainResult> res;
        app.eng.find_chain(obj, res, args.get("region").as_str("anon"), levels, mh);
        std::string s = "起点矩阵对象 = " + hex_of(obj) + "\n";
        for (auto &r : res)
        {
            s += r.reached_mod
               ? "追到模块: " + r.mod_name + " 基址=" + hex_of(r.mod_base) + "\n"
               : "未追到模块段（可能级数不够或指针在其它区域）\n";
            s += "共 " + std::to_string(r.levels.size()) + " 级反查：\n";
            for (size_t i = 0; i < r.levels.size(); ++i)
            {
                auto &cl = r.levels[i];
                char ln[256];
                if (cl.from_base)
                    snprintf(ln, sizeof(ln), "  L%zu holder=%s  value=%s  基址偏移=+0x%llx\n",
                             i, hex_of(cl.holder).c_str(), hex_of(cl.value).c_str(),
                             (unsigned long long)cl.from_base);
                else
                    snprintf(ln, sizeof(ln), "  L%zu holder=%s  value=%s  (相对上级 +0x%llx)\n",
                             i, hex_of(cl.holder).c_str(), hex_of(cl.value).c_str(),
                             (unsigned long long)cl.from_prev);
                s += ln;
            }
        }
        return ok_text(s);
    }

    // ── 暂停/恢复 ──
    if (name == "gg_pause" || name == "gg_resume")
    {
        pid_t p = args.has("pid") ? (pid_t)args.get("pid").as_int() : d.target_pid();
        if (p <= 0)
            return err_text("无目标 pid");
        int sig = (name == "gg_pause") ? SIGSTOP : SIGCONT;
        if (kill(p, sig) != 0)
            return err_text("信号发送失败");
        return ok_text(name == "gg_pause" ? "已暂停" : "已恢复");
    }

    return err_text("未知工具: " + name);
}

// ── MCP JSON-RPC 分发 ────────────────────────────────────────────────────
static mj::Value handle_rpc(App &app, const mj::Value &req)
{
    mj::Value resp = mj::Value::make_obj();
    resp.set("jsonrpc", "2.0");

    // id 原样回传（存在才带）
    if (req.has("id"))
        resp.set("id", req.get("id"));

    std::string method = req.get("method").as_str();

    if (method == "initialize")
    {
        mj::Value result = mj::Value::make_obj();
        result.set("protocolVersion", "2024-11-05");
        mj::Value caps = mj::Value::make_obj();
        mj::Value tools = mj::Value::make_obj();
        caps.set("tools", tools);
        result.set("capabilities", caps);
        mj::Value si = mj::Value::make_obj();
        si.set("name", "twtmcp");
        si.set("version", kVersion);
        result.set("serverInfo", si);
        resp.set("result", result);
        return resp;
    }

    if (method == "notifications/initialized")
    {
        // 通知无响应体，但 HTTP 下回个空 result 也无害
        resp.set("result", mj::Value::make_obj());
        return resp;
    }

    if (method == "tools/list")
    {
        mj::Value result = mj::Value::make_obj();
        mj::Value arr = mj::Value::make_arr();
        for (auto &t : kTools)
        {
            mj::Value item = mj::Value::make_obj();
            item.set("name", t.name);
            item.set("description", t.desc);
            item.set("inputSchema", tool_schema(t.name));
            arr.push(item);
        }
        result.set("tools", arr);
        resp.set("result", result);
        return resp;
    }

    if (method == "tools/call")
    {
        std::string name = req.get("params").get("name").as_str();
        const mj::Value &args = req.get("params").get("arguments");

        std::lock_guard<std::mutex> lk(app.mtx);
        mj::Value result;
        try
        {
            result = call_tool(app, name, args);
        }
        catch (...)
        {
            result = err_text("工具执行异常: " + name);
        }
        resp.set("result", result);
        return resp;
    }

    // ping
    if (method == "ping")
    {
        resp.set("result", mj::Value::make_obj());
        return resp;
    }

    // 未知方法
    mj::Value err = mj::Value::make_obj();
    err.set("code", -32601);
    err.set("message", "Method not found: " + method);
    resp.set("error", err);
    return resp;
}

int main(int argc, char **argv)
{
    std::string bind_ip = "0.0.0.0";
    int         port    = 8788;
    int         disc_port = 8789;
    std::string token;
    bool        discover_only = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if ((a == "-l" || a == "--listen") && i + 1 < argc)
        {
            std::string l = argv[++i];
            size_t c = l.find(':');
            if (c != std::string::npos)
            {
                bind_ip = l.substr(0, c);
                port    = atoi(l.substr(c + 1).c_str());
            }
            else
                bind_ip = l;
        }
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (a == "--disc-port" && i + 1 < argc)
            disc_port = atoi(argv[++i]);
        else if (a == "--token" && i + 1 < argc)
            token = argv[++i];
        else if (a == "--discover")
            discover_only = true;
        else if (a == "-h" || a == "--help")
        {
            printf("twtmcp %s —— TwT 驱动 MCP Server\n", kVersion);
            printf("用法: twtmcp [选项]\n");
            printf("  -l, --listen IP:PORT   监听地址（默认 0.0.0.0:8788）\n");
            printf("  --port PORT            HTTP 端口\n");
            printf("  --disc-port PORT       UDP 发现端口（默认 8789）\n");
            printf("  --token TOKEN          启用鉴权（Header X-TWT-Token 或 ?token=）\n");
            printf("  --discover             仅扫描局域网节点后退出\n");
            return 0;
        }
    }

    // 仅发现模式
    if (discover_only)
    {
        printf("[*] 扫描局域网 twtmcp 节点（UDP %d，约 1.2s）...\n", disc_port);
        auto nodes = lan_discover(disc_port, 1200);
        if (nodes.empty())
            printf("(未发现节点)\n");
        for (auto &n : nodes)
            printf("  %s:%d  name=%s ver=%s pid=%d\n", n.ip.c_str(), n.http_port,
                   n.name.c_str(), n.version.c_str(), n.target_pid);
        return 0;
    }

    // 构造节点信息
    char host[128] = "twtmcp";
    gethostname(host, sizeof(host) - 1);

    App app;

    if (!app.drv.ready())
    {
        fprintf(stderr, "[!] TwT 驱动未连接。请先注入 TwT 内核方案。\n");
        fprintf(stderr, "[!] server 仍会启动，但内存工具会失败。\n");
    }

    // HTTP handler
    HttpServer http(bind_ip, port);
    http.set_handler([&](const HttpReq &r) -> HttpResp {
        // 鉴权
        if (!token.empty())
        {
            std::string got = r.header("x-twt-token");
            if (got.empty())
            {
                // 兜底：query 参数 ?token=xxx
                size_t q = r.path.find("token=");
                if (q != std::string::npos)
                {
                    got = r.path.substr(q + 6);
                    size_t amp = got.find('&');
                    if (amp != std::string::npos)
                        got = got.substr(0, amp);
                }
            }
            if (got != token)
                return HttpResp::json("{\"error\":\"unauthorized\"}", 401);
        }

        // 健康检查
        if (r.path.rfind("/health", 0) == 0)
            return HttpResp::json("{\"status\":\"ok\",\"driver\":" +
                                  std::string(app.drv.ready() ? "true" : "false") +
                                  ",\"pid\":" + std::to_string(app.drv.target_pid()) + "}");

        // MCP 端点
        if (r.path.rfind("/mcp", 0) == 0 && r.method == "POST")
        {
            mj::Value req;
            if (!mj::parse(r.body, req))
                return HttpResp::json("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"parse error\"}}", 200);

            mj::Value resp = handle_rpc(app, req);
            return HttpResp::json(resp.dump());
        }

        return HttpResp::text("twtmcp: POST /mcp", 200);
    });

    if (!http.start())
    {
        fprintf(stderr, "[!] 无法监听 %s:%d\n", bind_ip.c_str(), port);
        return 1;
    }

    // UDP 发现服务
    LanDiscovery lan;
    lan.start([&]() -> LanInfo {
        return LanInfo{host, kVersion, port, disc_port, app.drv.target_pid()};
    }, disc_port);

    printf("[+] twtmcp %s 已启动\n", kVersion);
    printf("    MCP 端点 : http://%s:%d/mcp\n", bind_ip.c_str(), port);
    printf("    局域网   : http://<本机IP>:%d/mcp\n", port);
    printf("    发现服务 : UDP %d (TWT_DISCOVER)\n", disc_port);
    printf("    驱动状态 : %s\n", app.drv.ready() ? "已连接" : "未连接");
    printf("    鉴权     : %s\n", token.empty() ? "关闭" : "开启 (X-TWT-Token)");
    fflush(stdout);

    // 常驻
    while (true)
        sleep(3600);

    return 0;
}
