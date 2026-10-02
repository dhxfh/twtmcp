#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 4 字节精度扫矩阵对象区，用相机自洽判据定位真 ViewProjection。"""
import subprocess, struct, json, os, re, sys

os.environ['MSYS_NO_PATHCONV'] = '1'
PY = r"C:/Users/Administrator/.workbuddy-ai/binaries/python/versions/3.13.12/python.exe"
CWD = r"D:/免费ai/2026-10-01-18-37-44/twtmcp"


def call(tool, args):
    p = subprocess.run([PY, 'tools/mcp.py', 'call', tool, json.dumps(args)],
                       capture_output=True, text=True, cwd=CWD)
    return p.stdout + p.stderr


def dump(addr, size):
    data = bytearray()
    step = 0x1000
    for base in range(0, size, step):
        n = min(step, size - base)
        out = call("gg_hex_dump", {"address": hex(addr + base), "size": n})
        for line in out.splitlines():
            m = re.match(r'^\s*(0x[0-9a-fA-F]+):\s+(.*)$', line)
            if not m:
                continue
            bs = [b for b in m.group(2).split() if re.fullmatch(r'[0-9a-fA-F]{2}', b)]
            data += bytes(int(b, 16) for b in bs)
    return bytes(data)


if __name__ == '__main__':
    base = int(sys.argv[1], 0)
    size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x2000
    cam = (30586.436, 29792.820, 155.194)
    if len(sys.argv) > 5:
        cam = (float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]))
    buf = dump(base, size)
    print("dump %#x size=%d got=%d  cam=%s" % (base, size, len(buf), cam))
    X, Y, Z = cam
    hits = []
    for off in range(0, len(buf) - 64, 4):
        f = struct.unpack_from('<16f', buf, off)
        if any(x != x or abs(x) > 1e8 for x in f):
            continue
        wrow = abs(f[12]) + abs(f[13]) + abs(f[14]) + abs(f[15])
        if wrow < 1e-2:
            continue
        w = X * f[12] + Y * f[13] + Z * f[14] + f[15]
        if not (w > 1e-3):
            continue
        cx = X * f[0] + Y * f[1] + Z * f[2] + f[3]
        cy = X * f[4] + Y * f[5] + Z * f[6] + f[7]
        nx, ny = cx / w, cy / w
        if abs(nx) > 0.05 or abs(ny) > 0.05:
            continue
        hits.append((off, w, nx, ny, f))
    print("命中 %d 个（4 字节对齐）" % len(hits))
    for off, w, nx, ny, f in hits:
        print("  +%#06x  addr=%#x  w=%.3f ndc=(%.5f,%.5f)" % (off, base + off, w, nx, ny))
        for r in range(4):
            print("        " + " ".join("%13.5f" % f[r * 4 + c] for c in range(4)))
