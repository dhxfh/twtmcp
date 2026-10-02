import json, subprocess, struct, os, sys, urllib.request

os.chdir(r'D:/免费ai/2026-10-01-18-37-44/twtmcp')
PORT = 8798
TOKEN = 'twt2026'

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout

def ensure_fwd():
    sh('adb start-server')
    if f'tcp:{PORT}' not in sh('adb forward --list'):
        sh(f'adb forward tcp:{PORT} tcp:{PORT}')

def call(name, args):
    ensure_fwd()
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(
        f'http://127.0.0.1:{PORT}/mcp', data=body,
        headers={'Content-Type': 'application/json', 'X-TWT-Token': TOKEN})
    with urllib.request.urlopen(req, timeout=600) as r:
        d = json.loads(r.read())
    return d['result']['content'][0]['text']

def rd(addr, size=64):
    t = call('gg_read_bytes', {"address": addr, "size": size})
    hexs = ''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    b = bytes.fromhex(hexs.replace(' ', ''))
    return list(struct.unpack('<%df' % (len(b) // 4), b))

VIEW = sys.argv[1] if len(sys.argv) > 1 else '0x76023d0650'
PROJ = sys.argv[2] if len(sys.argv) > 2 else '0x7602414754'

v = rd(VIEW)
p = rd(PROJ)

print('VIEW @', VIEW, ' (行主序，末行为平移)')
for r in range(4):
    print('  ' + ' '.join(f'{v[r*4+c]:12.6f}' for c in range(4)))
print('PROJ @', PROJ, ' (列主序)')
for r in range(4):
    print('  ' + ' '.join(f'{p[r*4+c]:12.6f}' for c in range(4)))
print()
print(f'VIEW[15]={v[15]:.6f}  相机世界坐标=({v[12]:.3f}, {v[13]:.3f}, {v[14]:.3f})')
