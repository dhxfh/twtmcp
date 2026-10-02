# twtmcp —— TwT 驱动 MCP Server（局域网可访问）

把 **TwT 内核驱动**的内存读写能力通过 **MCP（Model Context Protocol）** 暴露给 AI 客户端。
参考 [gameguardian-mcp-server](https://github.com/giaoimgiao/gameguardian-mcp-server) 的 26 工具设计，
用 **C++** 复刻，驱动后端换成 **TwT**（`twt/kernel.h` 协议），传输层扩展为**局域网可访问 + UDP 设备发现**。

```
┌─────────────┐  HTTP JSON-RPC  ┌──────────────┐  ioctl('T',4/5)  ┌─────────────┐
│ AI / MCP    │ ──────────────▶ │  twtmcp      │ ───────────────▶ │ TwT Driver  │
│ Client      │ ◀────────────── │  (C++, root) │ ◀─────────────── │ (KPM)       │
└─────────────┘  :8788/mcp      └──────┬───────┘                  └─────────────┘
       ▲                               │ UDP :8789 TWT_DISCOVER
       │  局域网其它设备/控制器 ◀────────┘
```

---

## 一、与参考项目的差异

| | gameguardian-mcp-server | twtmcp（本项目） |
|---|---|---|
| 语言 | Go | C++（复用 `twt/kernel.h` 的 `c_driver`） |
| 驱动 | MemoryDriver KPM（ioctl 0x7e1a/0x7e1b） | TwT（ioctl `_IOW('T',4/5)`，`MY_CALL` 装 fd） |
| 监听 | 127.0.0.1:8788 | **0.0.0.0:8788（局域网）** |
| 鉴权 | 无 | **X-TWT-Token / ?token=** |
| 发现 | 无 | **UDP 8789 广播（TWT_DISCOVER → TWT_HERE）** |
| 工具 | 26 个 | 26 个（同名同语义） |

---

## 二、28 个 MCP 工具

**进程管理**：`gg_list_processes` `gg_attach` `gg_detach` `gg_target_info` `gg_module_list` `gg_module_base`
**内存搜索**：`gg_search` `gg_search_range` `gg_search_unknown_init` `gg_search_refine` `gg_result_count` `gg_get_results` `gg_clear_results`
**读写修改**：`gg_read` `gg_read_bytes` `gg_hex_dump` `gg_write` `gg_set_results`
**冻结**：`gg_freeze` `gg_unfreeze` `gg_freeze_list`
**分析定位**：`gg_calc_offset` `gg_pointer_scan` `gg_scan_matrix`
**游戏数据（DFM 实测）**：`gg_view_matrix`（视图投影矩阵）`gg_camera`（相机位置/旋转/FOV）`gg_maps`（内存段诊断）
**进程控制**：`gg_pause` `gg_resume`
**会话**：`gg_status`

---

## 二·补、DFM 视图矩阵链（实测确认）

《三角洲行动》(`com.tencent.tmgp.dfm`) 的**视图投影矩阵（ViewProjection）**读取链：

```
矩阵地址 = *(*((libUE4.so_base) + 0x202EE508) + 0xCC0) + 0xB28
             └─ 第一级 ─┘  └─ 0xCC0 ─┘  └─ 0xB28，直接是矩阵地址，不再解引用
```

- **布局：行主序（row-major）**，16 个 float。
- 世界坐标 → 屏幕（行主序约定）：
  ```
  w  = x*M[12] + y*M[13] + z*M[14] + M[15]
  cx = x*M[0]  + y*M[1]  + z*M[2]  + M[3]
  cy = x*M[4]  + y*M[5]  + z*M[6]  + M[7]
  screenX = W/2 + (cx/w) * W/2
  screenY = H/2 - (cy/w) * H/2
  ```
- **自洽校验**：把相机自身世界位置投入该矩阵，应落在屏幕中心（实测 2400×1080 → (1200.05, 540.04)）。
- `gg_view_matrix` 已内置该校验。相机位置由 `gg_camera` 读取：
  ```
  UWorld    = *(libUE4 + 0x205F5B50)
  本地控制器 = UWorld+0x190 -> +0x38 -> +0x0 -> +0x30
  玩家相机   = 本地控制器 + 0x408
  位置/旋转/FOV = 玩家相机 + 0x37C0 起 0x20 字节
  ```

### 独立扫描（不依赖指针链）：`gg_scan_matrix mode=5`

用「相机自洽」判据反扫矩阵，无需预知链：

```
gg_scan_matrix {"region":"all","mode":5,
                "addr_start":"0x...","addr_end":"0x...",   # 可选：限定段
                "cam_x":..,"cam_y":..,"cam_z":..,          # 不给则自动读相机
                "tol_ndc":0.05,"min_nonzero":4,"min_rows":2}
```

判据（行主序 ViewProjection）：
```
w  = X*M12 + Y*M13 + Z*M14 + M15        # 必须 > 0，不设上限
nx = (X*M0 + Y*M1 + Z*M2 + M3)/w        # |nx|,|ny| <= tol_ndc
ny = (X*M4 + Y*M5 + Z*M6 + M7)/w
```
实测：限定段 `77571d0000-77571e0000` 命中 `0x77571de670`，
第 4 行前 3 个 = 相机世界位置，与 `gg_camera` 逐位吻合。

**驱动读取限制（重要）**：TwT 驱动对**含空洞/未映射页的段**整块读会失败。
`gg_scan_matrix` 已改为**容错读**：整块读失败则逐 4KB 页读、失败页填 0 继续，
段内空洞不再导致整段被丢弃。`TWT_IO_CHUNK` 由 64KB 降为 4096。

---

## 三、目录结构

```
twtmcp/
├── src/
│   ├── twt_driver.hpp   ← TwT 驱动后端（无交互版 c_driver，协议对齐 kernel.h）
│   ├── engine.hpp       ← 内存搜索引擎（精确/模糊/区间/冻结/指针扫描）
│   ├── json.hpp         ← 极简 JSON 解析/生成（零依赖）
│   ├── http.hpp         ← 极简 HTTP/1.1 服务器（多线程）
│   ├── lan.hpp          ← UDP 设备发现 + 客户端扫描
│   └── main.cpp         ← MCP JSON-RPC 分发 + 26 工具实现 + main
├── build.sh             ← 交叉编译（aarch64）
├── deploy.sh            ← 推送+启动
├── build/twtmcp         ← 已编译产物（aarch64, 729472 字节）
└── README.md
```

---

## 四、编译

```bash
cd twtmcp
NDK=/c/Users/Administrator/android-ndk-r27d ./build.sh
```

已验证的单条命令：
```bash
NDK=/c/Users/Administrator/android-ndk-r27d
TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
SYSR=$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot
$TC/aarch64-linux-android28-clang++ --target=aarch64-linux-android28 --sysroot=$SYSR \
  -std=c++20 -O2 -static-libstdc++ -o build/twtmcp src/main.cpp -llog
```

**编译验证结论**（本机实测）：
- ELF aarch64，729472 字节，链接通过
- 反汇编确认 `mov w1,#0x5400 / #0x5404` + `movk w1,#0x4020,lsl#16`（= TwT `GET_PID` / `READ_MEM`）
- 含 `svc #0`（TwT 装 fd 的 reboot magic）
- 26 个工具名、`TWT_DISCOVER`/`TWT_HERE`/`/mcp`/`x-twt-token` 全部编入

---

## 五、安装（已实测落机）

**一键安装：**
```bash
cd twtmcp
./install.sh                       # 默认 port 8798, token twt2026
PORT=9000 TOKEN=abc ./install.sh   # 自定义
```

**实测结果（设备 NP05J / Android 16 / kernel 6.6.89 / aarch64）：**

```
=== 1. 检查设备 ===     设备: NP05J
=== 2. 检查 root ===    root OK  (uid=0)
=== 3. 检查 TwT 驱动 === TwT_driver 已注册  (anon_inode:TwT_driver)
=== 6. 启动 ===
[twt] 驱动对接成功 fd=3
[+] twtmcp 1.0.0 已启动
    MCP 端点 : http://0.0.0.0:8798/mcp
    驱动状态 : 已连接
    鉴权     : 开启 (X-TWT-Token)
```

**真实内存读写已通过**（走 TwT ioctl，非纸面）：
```bash
# attach system_server
gg_attach {package:"system_server"}   → 已附加 pid=3581

# 读 libc.so 基址
gg_module_base {module:"libc.so"}     → 0x79f2e3a000

# 读该地址 16 字节
gg_read_bytes {address:"0x79f2e3a000", size:16}
   → 7f 45 4c 46 02 01 01 00 ...      (ELF 魔数 ✓)

# 对照设备 /proc/3581/maps
79f2e3a000-79f2e95000 r--p ... /apex/com.android.runtime/lib64/bionic/libc.so  ← 地址完全一致
```

**协调说明**：设备上原本已有 Go 版 `ggmcp` 占着 8788（那套是给 MemoryDriver KPM 写的，
本机无 KPatch，走的是后备通道）。twtmcp 用 **8798**（HTTP）+ **8799**（UDP 发现）避开冲突，
两者可共存；要纯 TwT 内核通道就用 twtmcp。

**开机自启**：已装 `/data/adb/service.d/twtmcp.sh`（等 boot_completed + 等 TwT 驱动注册后再拉起）。

**MCP 配置**（已写入 `~/.workbuddy-ai/mcp.json`）：
```json
"twtmcp": {
  "type": "http",
  "url": "http://127.0.0.1:8798/mcp",
  "headers": { "X-TWT-Token": "twt2026" }
}
```
> 在 WorkBuddy 连接器管理页右上角「自定义连接器」对该 MCP 点「信任」即可启用。

---

## 六、MCP 客户端配置

**stdio 不便时用 HTTP：**
```json
{
  "mcpServers": {
    "twtmcp": {
      "type": "http",
      "url": "http://127.0.0.1:8788/mcp",
      "headers": { "X-TWT-Token": "mysecret" }
    }
  }
}
```
> 未启用 `--token` 时 headers 可省略。

---

## 七、局域网用法

**1. 发现节点（在一台机器上扫描）**
```bash
# 设备侧
/data/local/tmp/twtmcp --discover
# 或局域网内任意 Linux 机器
```
输出形如：
```
  192.168.1.23:8788  name=twtmcp ver=1.0.0 pid=12345
```

**2. 远程控制（局域网内另一设备）**
```bash
curl -X POST http://192.168.1.23:8788/mcp \
  -H 'Content-Type: application/json' \
  -H 'X-TWT-Token: mysecret' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"gg_attach","arguments":{"package":"com.xxx"}}}'
```

**3. 调用示例（本机）**
```bash
# 附加进程
curl -s http://127.0.0.1:8788/mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
  "params":{"name":"gg_attach","arguments":{"package":"com.tencent.tmgp.dfm"}}}'

# 搜金币
curl -s http://127.0.0.1:8788/mcp -d '{"jsonrpc":"2.0","id":2,"method":"tools/call",
  "params":{"name":"gg_search","arguments":{"value":"100","type":"dword","region":"anon"}}}'

# 批量改成 99999
curl -s http://127.0.0.1:8788/mcp -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"gg_set_results","arguments":{"type":"dword",
    "values":[{"index":0,"value":"99999"}]}}}'
```

---

## 八、命令行参数

| 参数 | 说明 |
|---|---|
| `-l, --listen IP:PORT` | 监听地址（默认 `0.0.0.0:8788`） |
| `--port PORT` | HTTP 端口 |
| `--disc-port PORT` | UDP 发现端口（默认 8789） |
| `--token TOKEN` | 启用鉴权（`X-TWT-Token` 头或 `?token=`） |
| `--discover` | 仅扫描局域网节点后退出 |
| `-h, --help` | 帮助 |

---

## 九、协议细节（与 TwT kernel.h 对齐）

```
sizeof(twt_request) = 32  (pid_t + pad + uintptr_t + void* + size_t)

GET_PID      = _IOW('T', 0,  req) = 0x40205400
MODULE_BASE  = _IOW('T', 1,  req) = 0x40205401
MODULE_BSS   = _IOW('T', 3,  req) = 0x40205403
READ_MEM     = _IOW('T', 4,  req) = 0x40205404
WRITE_MEM    = _IOW('T', 5,  req) = 0x40205405
READ_MEM_V2  = _IOW('T', 11, req) = 0x4020540b

装 fd: MY_CALL(0x114514, 0x1919810, 0x2778, &fd)   // 寄存器直呼 svc #0
识别: /proc/self/fd -> anon_inode:TwT_driver
```

单次 ioctl 上限 64KB，超出自动分块（`TWT_IO_CHUNK`）。

---

## 十、未执行 / 待验证

- **未在真机运行**（本机 Windows，无 Android 设备）：读写/搜索/冻结/局域网收发均未实测。
- 未验证 TwT 各内核方案下 `TwT_driver` 的实际注册时机。
- 搜索性能未压测（当前为朴素逐块扫描，1MB/块）。

上机首验（attach 后读模块基址，返回非 0 即通）：
```bash
curl -s http://127.0.0.1:8788/mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
  "params":{"name":"gg_module_base","arguments":{"module":"libc.so"}}}'
```

---

## 十一、安全说明

- 局域网模式默认无鉴权，**上线务必加 `--token`**，否则同网段任何设备都能读写目标进程内存。
- 建议仅在可信内网使用；不要在公网暴露 8788/8789。

---

## 十二、文件清单

```
twtmcp/
├── src/{twt_driver,engine,json,http,lan}.hpp + main.cpp
├── build/twtmcp          # aarch64 二进制（729472 B）
├── build.sh              # 交叉编译
├── deploy.sh             # 手动部署
├── install.sh            # 一键安装（推送+启动+转发+自测+自启）
└── twtmcp-boot.sh        # 开机自启模板
```
