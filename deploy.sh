#!/usr/bin/env bash
# ============================================================================
# deploy.sh —— 推送并启动 twtmcp（Android，需 root）
# ============================================================================
# 用法：
#   ./deploy.sh                 # 默认 0.0.0.0:8788，无鉴权
#   ./deploy.sh --token s3cret  # 带 token
#   ./deploy.sh --port 9000
# ============================================================================
set -e

BIN="${BIN:-build/twtmcp}"
ARGS="$@"
REMOTE=/data/local/tmp/twtmcp

[ -f "$BIN" ] || { echo "[!] 找不到 $BIN，先跑 ./build.sh"; exit 1; }

echo "[*] 推送 $BIN -> $REMOTE"
adb push "$BIN" "$REMOTE"

echo "[*] 启动（root）"
adb shell su -c "chmod 755 $REMOTE"

# 先停旧的
adb shell su -c "pkill -f twtmcp 2>/dev/null; sleep 0.3" || true

# 后台常驻
adb shell su -c "setsid nohup $REMOTE $ARGS > /data/local/tmp/twtmcp.log 2>&1 < /dev/null &"
sleep 1

echo "[*] 日志："
adb shell su -c "tail -20 /data/local/tmp/twtmcp.log"

echo
echo "[*] 端口检查："
adb shell su -c "netstat -tlnp 2>/dev/null | grep 8788 || ss -tlnp 2>/dev/null | grep 8788" || true

echo
IP=$(adb shell "ip route 2>/dev/null | grep -oE 'src [0-9.]+' | head -1 | cut -d' ' -f2" | tr -d '\r')
echo "[+] 局域网 MCP 端点: http://${IP:-<设备IP>}:8788/mcp"
echo "[+] 本机通过 adb 转发: adb forward tcp:8788 tcp:8788 -> http://127.0.0.1:8788/mcp"
