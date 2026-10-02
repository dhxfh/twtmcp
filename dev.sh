#!/usr/bin/env bash
# dev.sh —— 一次会话内完成：确保转发 + 发 MCP 请求
# 用法: ./dev.sh '<json-body>'  或  ./dev.sh tool <name> '<args-json>'
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
PORT="${PORT:-8798}"
TOKEN="${TOKEN:-twt2026}"

ensure() {
    adb start-server >/dev/null 2>&1
    if ! adb forward --list 2>/dev/null | grep -q "tcp:$PORT"; then
        adb forward tcp:$PORT tcp:$PORT >/dev/null 2>&1
    fi
}

post() {
    ensure
    curl -s --max-time 600 -X POST "http://127.0.0.1:$PORT/mcp" \
        -H "Content-Type: application/json" \
        -H "X-TWT-Token: $TOKEN" \
        -d "$1"
    echo
}

if [ "$1" = "tool" ]; then
    NAME="$2"; ARGS="${3:-{}}"
    post "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$NAME\",\"arguments\":$ARGS}}"
else
    post "$1"
fi
