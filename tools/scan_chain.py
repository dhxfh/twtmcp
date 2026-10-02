#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""独立扫描视图矩阵并反查完整指针链。"""
import subprocess, struct, json, os, re, sys

os.environ['MSYS_NO_PATHCONV'] = '1'
PY = r"C:/Users/Administrator/.workbuddy-ai/binaries/python/versions/3.13.12/python.exe"
CWD = r"D:/免费ai/2026-10-01-18-37-44/twtmcp"


def call(tool, args):
    p = subprocess.run([PY, 'tools/mcp.py', 'call', tool, json.dumps(args)],
                       capture_output=True, text=True, cwd=CWD)
    return p.stdout + p.stderr


def dump(addr, size):
    out = call("gg_hex_dump", {"address": hex(addr), "size": size})
    data = bytearray()
    for line in out.splitlines():
        m = re.match(r'^\s*(0x[0-9a-fA-F]+):\s+(.*)$', line)
        if not m:
            continue
        hexpart = m.group(2).strip().rstrip()
        bytes_ = [b for b in hexpart.split() if re.fullmatch(r'[0-9a-fA-F]{2}', b)]
        data += bytes(int(b, 16) for b in bytes_)
    return bytes(data)


def floats(buf, off, n=16):
    return struct.unpack_from('<%df' % n, buf, off)


if __name__ == '__main__':
    base = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x77571ddb88
    size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x1000
    buf = dump(base, size)
    print("dump %#x size=%d got=%d" % (base, size, len(buf)))
    # 每 0x10 对齐扫 4×4 float，打印非平凡块
    for off in range(0, len(buf) - 64, 0x10):
        f = floats(buf, off)
        nz = sum(1 for x in f if abs(x) > 1e-6)
        if nz < 8:
            continue
        big = max(abs(x) for x in f)
        if big > 1e7:
            continue
        print("---- off=+%#x addr=%#x nz=%d" % (off, base + off, nz))
        for r in range(4):
            print("   " + " ".join("%12.5f" % f[r * 4 + c] for c in range(4)))
