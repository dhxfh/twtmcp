import json, subprocess, struct, os, sys, urllib.request

os.chdir(r'D:/免费ai/2026-10-01-18-37-44/twtmcp')
PORT = 8798
TOKEN = 'twt2026'

def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout

def ensure():
    sh('adb start-server')
    if f'tcp:{PORT}' not in sh('adb forward --list'):
        sh(f'adb forward tcp:{PORT} tcp:{PORT}')

def call(n, a):
    ensure()
    b = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": n, "arguments": a}}).encode()
    rq = urllib.request.Request(f'http://127.0.0.1:{PORT}/mcp', data=b,
                                headers={'Content-Type': 'application/json',
                                         'X-TWT-Token': TOKEN})
    with urllib.request.urlopen(rq, timeout=600) as r:
        return json.loads(r.read())['result']['content'][0]['text']

def rd_bytes(addr, size):
    t = call('gg_read_bytes', {"address": hex(addr), "size": size})
    if '读失败' in t:
        return None
    hx = ''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ', ''))

def rd_u64(addr):
    b = rd_bytes(addr, 8)
    if not b: return None
    return struct.unpack('<Q', b)[0]

def rd_f32s(addr, n):
    b = rd_bytes(addr, n*4)
    if not b: return None
    return list(struct.unpack('<%df' % n, b))

def valid(p):
    return p is not None and 0x10000000 <= p < 0x10000000000

# 1) 取 libUE4.so 模块基址
base_s = call('gg_module_base', {"module": "libUE4.so"}).strip()
print('libUE4.so 基址(原始返回):', repr(base_s))
# 解析 "0x..." —— 响应是 JSON 包裹，这里直接取最后一段 hex
import re
m = re.search(r'0x[0-9a-fA-F]+', base_s)
BASE = int(m.group(0), 16)
print('libUE4.so 基址 =', hex(BASE))

OFF_GMATRIX2 = 0x202EE508
OFF_L1 = 0xCC0
OFF_L2 = 0xB28

# 2) 逐级解引用
a1 = BASE + OFF_GMATRIX2
print(f'\n[1] 基址头+0x202EE508 = {hex(a1)}')
p1 = rd_u64(a1)
print(f'    *  -> {hex(p1) if p1 else None}   valid={valid(p1)}')

if valid(p1):
    a2 = p1 + OFF_L1
    print(f'[2] +0xCC0 = {hex(a2)}')
    p2 = rd_u64(a2)
    print(f'    *  -> {hex(p2) if p2 else None}   valid={valid(p2)}')

    if valid(p2):
        a3 = p2 + OFF_L2
        print(f'[3] 矩阵地址 = {hex(a3)}')
        mv = rd_f32s(a3, 16)
        print('    16 float:')
        if mv:
            for r in range(4):
                print('      ' + ' '.join(f'{mv[r*4+c]:14.6f}' for c in range(4)))
