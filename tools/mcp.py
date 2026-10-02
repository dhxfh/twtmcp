#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mcp.py —— twtmcp 客户端封装（自愈 adb forward + 直连 127.0.0.1:8798）
用法:
    python tools/mcp.py list                 # 列工具
    python tools/mcp.py call <tool> '<json>' # 调工具
    python tools/mcp.py vp                   # 快捷: 读视图矩阵
    python tools/mcp.py cam                  # 快捷: 读相机
注意: 不依赖局域网，走 adb forward，daemon 重启后自动重建。
"""
import json, subprocess, sys, os, urllib.request

PORT = 8798
TOKEN = 'twt2026'
URL = f'http://127.0.0.1:{PORT}/mcp'


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout


def ensure():
    """幂等重建 adb forward（adb daemon 重启会清掉转发）。"""
    sh('adb start-server')
    if f'tcp:{PORT}' not in sh('adb forward --list'):
        sh(f'adb forward tcp:{PORT} tcp:{PORT}')


def rpc(method, params):
    ensure()
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                       "params": params}).encode()
    rq = urllib.request.Request(URL, data=body,
                                headers={'Content-Type': 'application/json',
                                         'X-TWT-Token': TOKEN})
    with urllib.request.urlopen(rq, timeout=120) as r:
        return json.loads(r.read())


def call(name, args):
    d = rpc("tools/call", {"name": name, "arguments": args})
    return d['result']['content'][0]['text']


def tool_list():
    d = rpc("tools/list", {})
    return [t['name'] for t in d['result']['tools']]


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] == 'list':
        for n in tool_list():
            print(n)
    elif a[0] == 'call':
        print(call(a[1], json.loads(a[2]) if len(a) > 2 else {}))
    elif a[0] == 'vp':
        print(call('gg_view_matrix', {}))
    elif a[0] == 'cam':
        print(call('gg_camera', {}))
    elif a[0] == 'attach':
        pid = a[1] if len(a) > 1 else '28702'
        print(call('gg_attach', {'pid': int(pid)}))
    else:
        print(__doc__)
