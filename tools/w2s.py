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
    t = call('gg_read_bytes', {"address": hex(addr), "size": size})
    hx = ''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ', ''))

VIEW_ADDR = 0x76023d0650
PROJ_ADDR = 0x7608b6cd5c
W, H = 2400, 1080

f = struct.unpack('<16f', raw(VIEW_ADDR, 64))
# 行主序：左上 3x3 旋转 R，末行平移 T
R = [[f[0], f[1], f[2]], [f[4], f[5], f[6]], [f[8], f[9], f[10]]]
T = [f[12], f[13], f[14]]
print('R =')
for r in R: print('  ' + ' '.join(f'{x:12.6f}' for x in r))
print('T =', [f'{x:.3f}' for x in T])

p = struct.unpack('<16f', raw(PROJ_ADDR, 64))
print('PROJ (行主序):')
for r in range(4): print('  ' + ' '.join(f'{p[r*4+c]:12.6f}' for c in range(4)))

# 世界 -> 相机：p_cam = R^T * (p_world - T)
def w2cam(pw):
    d = [pw[0]-T[0], pw[1]-T[1], pw[2]-T[2]]
    return [sum(R[i][k]*d[k] for k in range(3)) for i in range(3)]  # R^T * d

# 相机 -> NDC: 用行主序 P（前 3 行有效）
def cam2ndc(pc):
    v = [pc[0], pc[1], pc[2], 1.0]
    c = [sum(p[r*4+k]*v[k] for k in range(4)) for r in range(4)]
    if abs(c[3]) < 1e-9: return None
    return (c[0]/c[3], c[1]/c[3], c[2]/c[3])

def w2s(pw):
    n = cam2ndc(w2cam(pw))
    if not n: return None
    return n, (n[0]*0.5+0.5)*W, (1.0-(n[1]*0.5+0.5))*H

# 相机自身
for label, pw in [
    ('相机位置', T),
    ('前方+10', [T[0]+R[2][0]*10, T[1]+R[2][1]*10, T[2]+R[2][2]*10]),
    ('前方+100', [T[0]+R[2][0]*100, T[1]+R[2][1]*100, T[2]+R[2][2]*100]),
    ('右方+10',  [T[0]+R[0][0]*10, T[1]+R[0][1]*10, T[2]+R[0][2]*10]),
    ('上方+10',  [T[0]+R[1][0]*10, T[1]+R[1][1]*10, T[2]+R[1][2]*10]),
]:
    r = w2s(pw)
    if r:
        n, sx, sy = r
        print(f'{label:10s} NDC=({n[0]:8.4f},{n[1]:8.4f},{n[2]:8.4f})  屏幕=({sx:8.1f},{sy:8.1f})')
    else:
        print(f'{label:10s} 投影失败')
