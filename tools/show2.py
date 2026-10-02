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

def raw(addr, size):
    t = call('gg_read_bytes', {"address": addr, "size": size})
    hx = ''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ', ''))

def show(addr, size=64, label=''):
    b = raw(hex(addr), size)
    f = struct.unpack('<%df' % (len(b)//4), b)
    print(f'--- {label} {hex(addr)} ---')
    for r in range(len(f)//4):
        print('  ' + ' '.join(f'{f[r*4+c]:12.6f}' for c in range(4)))
    print()

VIEW = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x76023d0650
PROJ = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x7602414754

show(VIEW, 64, 'VIEW 行主序')
show(PROJ, 64, 'PROJ 行主序')
show(PROJ, 64, 'PROJ 同数据')

# 打印原始 hex 便于精确定位
b = raw(hex(PROJ), 64)
print('PROJ raw hex:', b.hex())
