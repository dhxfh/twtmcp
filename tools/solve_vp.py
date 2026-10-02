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

def mat4(data, off, rowmajor=True):
    f = struct.unpack_from('<16f', data, off)
    if rowmajor:
        return [list(f[r*4:r*4+4]) for r in range(4)]
    return [[f[c*4+r] for c in range(4)] for r in range(4)]

def mul(A, B):
    return [[sum(A[i][k]*B[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

def mulv(M, v):
    return [sum(M[i][k]*v[k] for k in range(4)) for i in range(4)]

VIEW_ADDR = 0x76023d0650
PROJ_ADDR = 0x7602414754

vdata = raw(hex(VIEW_ADDR), 64)
pdata = raw(hex(PROJ_ADDR - 256), 256 + 384)

results = []
for vrm in (True, False):
    v = mat4(vdata, 0, rowmajor=vrm)
    if vrm:
        cam = [v[3][0], v[3][1], v[3][2], 1.0]
        fwd = [v[2][0], v[2][1], v[2][2]]
    else:
        cam = [v[0][3], v[1][3], v[2][3], 1.0]
        fwd = [v[0][2], v[1][2], v[2][2]]
    ahead = [cam[0] + fwd[0]*100, cam[1] + fwd[1]*100, cam[2] + fwd[2]*100, 1.0]
    for off in range(0, len(pdata) - 64 + 1, 4):
        for rm in (True, False):
            P = mat4(pdata, off, rowmajor=rm)
            flat = [x for r in P for x in r]
            if max(abs(x) for x in flat) > 1e6:
                continue
            nz = sum(1 for x in flat if abs(x) > 1e-6)
            if nz < 6:
                continue
            if abs(flat[15]) > 1e-3:
                continue
            VP = mul(P, v)
            c1 = mulv(VP, cam)
            if abs(c1[3]) < 1e-9:
                continue
            n1 = [c1[0]/c1[3], c1[1]/c1[3]]
            c2 = mulv(VP, ahead)
            if c2[3] <= 0:
                continue
            n2 = [c2[0]/c2[3], c2[1]/c2[3]]
            err = abs(n1[0]) + abs(n1[1]) + abs(n2[0]) + abs(n2[1])
            results.append((err, hex(PROJ_ADDR - 256 + off), rm, vrm,
                            n1, n2, P, nz))

results.sort(key=lambda x: x[0])
print(f'共 {len(results)} 个候选，前 3:')
for r in results[:3]:
    err, addr, rm, vrm, n1, n2, P, nz = r
    print(f'\n起点={addr} PROJ行主序={rm} VIEW行主序={vrm} 非零={nz} 误差={err:.3e}')
    print('  相机NDC  =', [f'{x:.4f}' for x in n1])
    print('  前向NDC  =', [f'{x:.4f}' for x in n2])
    for row in P:
        print('    ' + ' '.join(f'{x:12.6f}' for x in row))

if results:
    err, addr, rm, vrm, n1, n2, P, nz = results[0]
    if err < 1e-3:
        print(f'\n*** 链条确认: VIEW=0x76023d0650(行主序={vrm})  PROJ={addr}(行主序={rm}) ***')

