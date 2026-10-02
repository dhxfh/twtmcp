#!/usr/bin/env bash
# ============================================================================
# install.sh —— 一键安装 twtmcp 到已连接设备 + 注册 MCP + 开机自启
# ============================================================================
# 前置：设备已 root，TwT 已注入（TwTLoaderv1.50.sh）
# 用法：
#   ./install.sh                 # 默认端口 8798，token twt2026
#   PORT=9000 TOKEN=abc ./install.sh
# ============================================================================
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"

PORT="${PORT:-8798}"
DISC_PORT="${DISC_PORT:-8799}"
TOKEN="${TOKEN:-twt2026}"
BIN="${BIN:-build/twtmcp}"
REMOTE=/data/local/tmp/twtmcp

[ -f "$BIN" ] || { echo "[!] 找不到 $BIN，先跑 ./build.sh"; exit 1; }

echo "=== 1. 检查设备 ==="
adb devices | grep -w device || { echo "[!] 无设备"; exit 1; }
MODEL=$(adb shell getprop ro.product.model | tr -d '\r')
echo "    设备: $MODEL"

echo "=== 2. 检查 root ==="
adb shell "su -c id" | grep -q "uid=0" || { echo "[!] 无 root"; exit 1; }
echo "    root OK"

echo "=== 3. 检查 TwT 驱动 ==="
if adb shell "su -c 'ls -la /proc/*/fd 2>/dev/null | grep -q TwT_driver'" ; then
    echo "    TwT_driver 已注册"
else
    echo "    [!] 警告：未检测到 TwT_driver，twtmcp 将无法读写内存"
    echo "        请先跑 TwTLoaderv1.50.sh 完成内核方案注入"
fi

echo "=== 4. 推送 ==="
adb push "$BIN" "$REMOTE"
adb shell "su -c 'chmod 755 $REMOTE'"

echo "=== 5. 停旧实例 ==="
adb shell "su -c 'pkill -f \"$REMOTE\" 2>/dev/null; sleep 0.5'" || true

echo "=== 6. 启动 ==="
adb shell "su -c 'setsid nohup $REMOTE --port $PORT --disc-port $DISC_PORT --token $TOKEN > /data/local/tmp/twtmcp.log 2>&1 < /dev/null &'"
sleep 2
adb shell "su -c 'cat /data/local/tmp/twtmcp.log'" | head -12

echo "=== 7. 建 adb 转发 ==="
adb forward "tcp:$PORT" "tcp:$PORT"
echo "    tcp:$PORT -> 设备:$PORT"

echo "=== 8. 自测 ==="
curl -s -m 5 -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H "X-TWT-Token: $TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | head -c 120
echo

echo "=== 9. 安装开机自启（可选，需 KernelSU service.d）==="
if [ -f twtmcp-boot.sh ]; then
    adb push twtmcp-boot.sh /data/local/tmp/twtmcp-boot.sh >/dev/null
    adb shell "su -c 'mkdir -p /data/adb/service.d && cp /data/local/tmp/twtmcp-boot.sh /data/adb/service.d/twtmcp.sh && chmod 755 /data/adb/service.d/twtmcp.sh'"
    echo "    已装 /data/adb/service.d/twtmcp.sh"
fi

echo
echo "[+] 安装完成"
echo "    MCP 配置已在 ~/.workbuddy-ai/mcp.json 注册为 twtmcp"
echo "    端点: http://127.0.0.1:$PORT/mcp   Token: $TOKEN"
echo
echo "[i] 在 WorkBuddy 连接器管理页右上角「自定义连接器」里对该 MCP 点「信任」以启用"
