import json,subprocess,struct,os,sys,urllib.request
os.chdir(r'D:/免费ai/2026-10-01-18-37-44/twtmcp')
PORT=8798; TOKEN='twt2026'
def sh(c): return subprocess.run(c,shell=True,capture_output=True,text=True).stdout
def ensure():
    sh('adb start-server')
    if f'tcp:{PORT}' not in sh('adb forward --list'): sh(f'adb forward tcp:{PORT} tcp:{PORT}')
def call(n,a):
    ensure()
    b=json.dumps({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":n,"arguments":a}}).encode()
    rq=urllib.request.Request(f'http://127.0.0.1:{PORT}/mcp',data=b,headers={'Content-Type':'application/json','X-TWT-Token':TOKEN})
    with urllib.request.urlopen(rq,timeout=600) as r: return json.loads(r.read())['result']['content'][0]['text']
def raw(addr,size):
    t=call('gg_read_bytes',{"address":addr,"size":size})
    hx=''.join(l.split(': ')[1] for l in t.strip().split('\n'))
    return bytes.fromhex(hx.replace(' ',''))
base=int(sys.argv[1],16)
# 读 base 前 32 字节 + 后 96 字节
data=raw(hex(base-32), 160)
start=base-32
for off in range(0,160,4):
    v=struct.unpack_from('<f',data,off)[0]
    mark='  <== 候选起点' if (start+off)==base else ''
    print(f'  {hex(start+off)}  {v:16.6f}{mark}')
