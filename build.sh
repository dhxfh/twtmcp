#!/usr/bin/env bash
# ============================================================================
# build.sh —— 交叉编译 twtmcp（aarch64 / Android）
# ============================================================================
# 用法：
#   ./build.sh                      # 用 PATH 里的 NDK 或自动探测
#   NDK=/path/to/android-ndk ./build.sh
# 产物：build/twtmcp（静态链 libc++，Android 上免依赖 libc++_shared.so）
# ============================================================================
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# 自动探测 NDK
if [ -z "$NDK" ]; then
    for c in "$HOME/android-ndk-r27d" "$HOME/android-ndk" /c/Users/Administrator/android-ndk-r27d \
             /opt/android-ndk /usr/local/android-ndk; do
        [ -d "$c" ] && NDK="$c" && break
    done
fi
[ -z "$NDK" ] && { echo "[!] 找不到 NDK，请设置 NDK=/path/to/android-ndk"; exit 1; }

PFX="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
[ -d "$PFX" ] || PFX="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[ -d "$PFX" ] || PFX="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
[ -d "$PFX" ] || { echo "[!] 找不到 toolchain 前缀: $NDK"; exit 1; }

CXX="$PFX/bin/aarch64-linux-android28-clang++"
SYSROOT="$PFX/sysroot"
[ -x "$CXX" ] || { echo "[!] 找不到 $CXX"; exit 1; }

echo "[*] NDK     : $NDK"
echo "[*] CXX     : $CXX"
echo "[*] 目标    : aarch64-linux-android28"

mkdir -p build
"$CXX" --target=aarch64-linux-android28 --sysroot="$SYSROOT" \
    -std=c++20 -O2 -static-libstdc++ \
    -o build/twtmcp src/main.cpp -llog

echo "[+] 产物: $HERE/build/twtmcp"
ls -la build/twtmcp
echo
echo "[i] 部署："
echo "    adb push build/twtmcp /data/local/tmp/"
echo "    adb shell su -c 'chmod 755 /data/local/tmp/twtmcp && /data/local/tmp/twtmcp'"
