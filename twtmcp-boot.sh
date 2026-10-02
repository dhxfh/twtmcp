#!/data/adb/service.d/twtmcp.sh
# ============================================================================
# twtmcp 开机自启（放到设备 /data/adb/service.d/ 下，chmod 755）
# ============================================================================
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 20   # 等 TwT 加载器把驱动注入完成

# 等 TwT 驱动注册（扫任何进程的 fd）
for i in $(seq 1 30); do
    if ls -la /proc/*/fd 2>/dev/null | grep -q TwT_driver; then
        break
    fi
    sleep 2
done

pkill -f '/data/local/tmp/twtmcp' 2>/dev/null
sleep 1

setsid nohup /data/local/tmp/twtmcp --port 8798 --disc-port 8799 --token twt2026 \
    > /data/local/tmp/twtmcp.log 2>&1 < /dev/null &
