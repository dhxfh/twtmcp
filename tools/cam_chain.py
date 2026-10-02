import json, subprocess, struct, os, re, urllib.request
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
def rd_bytes(addr,size):
    t=call('gg_read_bytes',{"address":hex(addr),"size":size})
    if '读失败' in t: return None
    hx=''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ',''))
def rd_u64(a):
    b=rd_bytes(a,8); return struct.unpack('<Q',b)[0] if b else None
def rd_f32s(a,n):
    b=rd_bytes(a,n*4); return list(struct.unpack('<%df'%n,b)) if b else None
def valid(p): return p is not None and 0x10000000<=p<0x10000000000

m=re.search(r'0x[0-9a-fA-F]+',call('gg_module_base',{"module":"libUE4.so"}))
BASE=int(m.group(0),16); print('libUE4.so 基址 =',hex(BASE))

# GWorld 链: GWorld -> UWorld ; PC/UObjectArray 等
GWorld = BASE + 0x205F5B50
uw = rd_u64(GWorld); print(f'GWorld({hex(GWorld)}) -> UWorld = {hex(uw) if uw else None}')

# 相机: 一般 LocalPlayerController + 0x408 -> PlayerCameraManager, +0x37C0 = 位置
# 但直接读 UWorld->OwningGameInstance->LocalPlayers[0]->PlayerController 太长, 先看能否从别处拿
# 尝试: UWorld + 0x38 (PersistentLevel) 不行. 直接扫相机位置附近
# 已知 相机_位置=0x37C0 相对 PlayerCameraManager. 先找 PlayerCameraManager:
# UWorld->OwningGameInstance(0x1A0)->LocalPlayers(0x38) 数组->[0](PlayerController)
for off in [0x1A0]:
    gi = rd_u64(uw+off) if uw else None
    print(f'  UWorld+{hex(off)} GameInstance = {hex(gi) if gi else None}')
    if gi and valid(gi):
        lpv = rd_u64(gi+0x38); print(f'    GameInstance+0x38 LocalPlayers(size) = {hex(lpv) if lpv else None}')
        pc = rd_u64(gi+0x38)  # TArray: data ptr at +0
        # TArray<ULocalPlayer*>: +0x0 = data ptr
        dp = rd_u64(gi+0x38)
        if dp and valid(dp):
            lp = rd_u64(dp); print(f'    LocalPlayer[0] = {hex(lp) if lp else None}')
            if lp and valid(lp):
                pc = rd_u64(lp+0x30); print(f'      LocalPlayer+0x30 PlayerController = {hex(pc) if pc else None}')
                if pc and valid(pc):
                    pcm = rd_u64(pc+0x408); print(f'        PC+0x408 PlayerCameraManager = {hex(pcm) if pcm else None}')
                    if pcm and valid(pcm):
                        pos = rd_f32s(pcm+0x37C0,3); rot = rd_f32s(pcm+0x37D0,3)
                        print(f'          相机位置 = {pos}')
                        print(f'          相机旋转 = {rot}')
                        fov = rd_f32s(pcm+0x37DC,1); print(f'          相机FOV  = {fov}')
