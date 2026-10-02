import json, subprocess, struct, os, re, urllib.request, math
os.chdir(r'D:/免费ai/2026-10-01-18-37-44/twtmcp')
PORT=8798; TOKEN='twt2026'
def sh(c): return subprocess.run(c,shell=True,capture_output=True,text=True).stdout
def ensure():
    sh('adb start-server')
    if f'tcp:{PORT}' not in sh('adb forward --list'): sh(f'adb forward tcp:{PORT} tcp:{PORT}')
def call(n,a):
    ensure()
    b=json.dumps({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":n,"arguments":a}}).encode()
    rq=urllib.request.Request(f'http://127.0.0.1:{PORT}/mcp',data=b,
        headers={'Content-Type':'application/json','X-TWT-Token':TOKEN})
    with urllib.request.urlopen(rq,timeout=600) as r: return json.loads(r.read())['result']['content'][0]['text']
def rd(addr,size):
    t=call('gg_read_bytes',{"address":hex(addr),"size":size})
    if '读失败' in t: return None
    hx=''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ',''))
def u64(a):
    b=rd(a,8); return struct.unpack('<Q',b)[0] if b else None
def f32s(a,n):
    b=rd(a,n*4); return list(struct.unpack('<%df'%n,b)) if b else None
def valid(p): return p is not None and 0x10000000<=p<0x10000000000

m=re.search(r'0x[0-9a-fA-F]+',call('gg_module_base',{"module":"libUE4.so"}))
BASE=int(m.group(0),16); print('libUE4.so 基址 =',hex(BASE))

# ---------- 1) GWorld -> UWorld ----------
uw = u64(BASE + 0x205F5B50)
print(f'UWorld = {hex(uw) if uw else None}')

# ---------- 2) 本地控制器链: UWorld+0x190 -> +0x38 -> +0x0 -> +0x30 ----------
p = uw
for off in [0x190, 0x38, 0x0, 0x30]:
    if not valid(p): break
    p = u64(p+off)
pc = p
print(f'本地控制器(PC) = {hex(pc) if pc else None}')

# ---------- 3) 玩家相机: PC+0x408 ----------
pcm = u64(pc+0x408) if valid(pc) else None
print(f'玩家相机(PCM) = {hex(pcm) if pcm else None}')

campos=None; camrot=None; fov=None
if valid(pcm):
    b = rd(pcm+0x37C0, 0x20)
    if b:
        campos=struct.unpack('<3f', b[0:12])
        camrot=struct.unpack('<3f', b[16:28])
        fov=struct.unpack('<f', b[28:32])[0]
        print(f'  相机位置 = ({campos[0]:.3f}, {campos[1]:.3f}, {campos[2]:.3f})')
        print(f'  相机旋转 = Pitch={camrot[0]:.3f} Yaw={camrot[1]:.3f} Roll={camrot[2]:.3f}')
        print(f'  相机FOV  = {fov:.3f}')

# ---------- 4) 视图矩阵链 ----------
g2 = u64(BASE+0x202EE508)
mid = u64(g2+0xCC0) if valid(g2) else None
mv = f32s(mid+0xB28,16) if valid(mid) else None
print(f'\n视图矩阵链: BASE+0x202EE508 -> {hex(g2) if g2 else None} -> +0xCC0 -> {hex(mid) if mid else None}')
if mv:
    print('  16 float:')
    for r in range(4): print('    '+' '.join(f'{mv[r*4+c]:13.6f}' for c in range(4)))

# ---------- 5) 投影自洽检验 ----------
if mv and campos:
    # 尝试两种布局, 用「相机位置应投到屏幕中心」判据
    f = mv
    px, py = 1200.0, 540.0   # 假设 2400x1080
    def proj(pos, M, colmajor):
        if colmajor:
            w  = M[3]*pos[0]+M[7]*pos[1]+M[11]*pos[2]+M[15]
            cx = M[0]*pos[0]+M[4]*pos[1]+M[8]*pos[2]+M[12]
            cy = M[1]*pos[0]+M[5]*pos[1]+M[9]*pos[2]+M[13]
        else:
            w  = M[12]*pos[0]+M[13]*pos[1]+M[14]*pos[2]+M[15]
            cx = M[0]*pos[0]+M[1]*pos[1]+M[2]*pos[2]+M[3]
            cy = M[4]*pos[0]+M[5]*pos[1]+M[6]*pos[2]+M[7]
        if abs(w)<1e-9: return None
        return (px+cx/w*px, py-cy/w*py)
    print('\n=== 投影自洽 (相机自身位置应落在屏幕中心 1200,540) ===')
    print('  列主序:', proj(campos,f,True))
    print('  行主序:', proj(campos,f,False))

    # 再测: 相机位置 + 前方一点 (旋转方向) 是否为正深度
    yaw=math.radians(camrot[1]); pitch=math.radians(camrot[0])
    fx=math.cos(pitch)*math.cos(yaw); fy=math.cos(pitch)*math.sin(yaw); fz=math.sin(pitch)
    ahead=(campos[0]+fx*1000, campos[1]+fy*1000, campos[2]+fz*1000)
    print('  相机前方1000单位:', proj(ahead,f,True), proj(ahead,f,False))
