#!/usr/bin/env bash
# ============================================================================
# keepalive.sh —— 保活 twtmcp：确保 adb 转发 + 设备进程都在
# ============================================================================
# adb forward 在 adb server 重启后会丢失。本脚本可反复执行（幂等），
# 也可以挂到计划任务里定时跑。
#
# 用法：
#   ./keepalive.sh
# ============================================================================
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"

PORT="${PORT:-8798}"
DISC_PORT="${DISC_PORT:-8799}"
TOKEN="${TOKEN:-twt2026}"
REMOTE=/data/local/tmp/twtmcp

echo "[*] 检查设备..."
DEV=$(adb devices 2>/dev/null | awk 'NR>1 && $2=="device"{print $1; exit}')
if [ -z "$DEV" ]; then
    echo "[!] 无已连接设备（adb devices 无 device）"
    exit 1
fi
echo "    设备: $DEV"

echo "[*] 检查设备进程..."
if adb shell "su -c 'ps -A -o NAME | grep -q twtmcp'" 2>/dev/null; then
    echo "    twtmcp 在跑"
else
    echo "    twtmcp 未运行，拉起..."
    adb push "$(dirname "$0")/build/twtmcp" "$REMOTE" >/dev/null 2>&1
    adb shell "su -c 'chmod 755 $REMOTE; setsid nohup $REMOTE --port $PORT --disc-port $DISC_PORT --token $TOKEN > /data/local/tmp/twtmcp.log 2>&1 < /dev/null &'"
    sleep 2
fi

echo "[*] 重建 adb 转发 tcp:$PORT..."
adb forward --remove "tcp:$PORT" 2>/dev/null
adb forward "tcp:$PORT" "tcp:$PORT" >/dev/null

echo "[*] 连通性测试..."
RESP=$(curl -s -m 5 -X POST "http://127.0.0.1:$PORT/mcp" \
    -H 'Content-Type: application/json' -H "X-TWT-Token: $TOKEN" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize"}' 2>&1)
if echo "$RESP" | grep -q twtmcp; then
    echo "[+] OK  http://127.0.0.1:$PORT/mcp  (token: $TOKEN)"
else
    echo "[!] 失败: $RESP"
    exit 1
fi
