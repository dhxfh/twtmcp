#!/usr/bin/env bash
# watch_matrix.sh —— 对比矩阵地址是否随镜头变化
# 用法: ./watch_matrix.sh 0xADDR1 0xADDR2 ...
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
cd "$(dirname "$0")"

for a in "$@"; do
    printf "%s : " "$a"
    ./dev.sh tool gg_read_bytes "{\"address\":\"$a\",\"size\":64}" \
      | grep -o '"text":"[^"]*"' | sed 's/\\n/ | /g' | cut -c1-200
    echo
done
