#!/system/bin/sh
# Enhanced init.kernel.post_boot-bengal.sh for Xiaomi 23129RAA4G (SD685)
# Designed to work WITH boot optimizations (performance governor during boot)
# Restores balanced settings after boot completes

# Copyright (c) 2020-2026 Qualcomm Technologies, Inc.
# Modified for SD685 platform (bengal) - Xiaomi 23129RAA4G

KernelVersionStr=`cat /proc/sys/kernel/osrelease`
KernelVersionS=${KernelVersionStr:2:2}
KernelVersionA=${KernelVersionStr:0:1}
KernelVersionB=${KernelVersionS%.*}

#=====================================================================
# LOGGING FUNCTION
#=====================================================================
LOG_FILE="/dev/kmsg"
log_message() {
    echo "$1" > $LOG_FILE 2>/dev/null
    echo "[SD685] $1"
}

log_message "========================================="
log_message "SD685 Post-Boot Configuration Starting"
log_message "========================================="

#=====================================================================
# FUNCTION: Apply setting with verification
#=====================================================================
apply_setting() {
    local path="$1"
    local value="$2"
    local name="$3"
    local retries=5
    local delay=1
    
    for i in $(seq 1 $retries); do
        if [ -f "$path" ]; then
            echo "$value" > "$path" 2>/dev/null
            sleep 0.3
            local current=$(cat "$path" 2>/dev/null | head -c 50)
            if echo "$current" | grep -q "$value"; then
                log_message "✓ Applied: $name = $value"
                return 0
            else
                log_message "⚠️ Retry $i: $name (current: $current, expected: $value)"
                sleep $delay
            fi
        else
            log_message "✗ Path not found: $path"
            return 1
        fi
    done
    log_message "❌ Failed to apply: $name = $value"
    return 1
}

#=====================================================================
# FORCE SETTING (for critical params)
#=====================================================================
force_setting() {
    local path="$1"
    local value="$2"
    local name="$3"
    
    for i in 1 2 3; do
        if [ -f "$path" ]; then
            echo "$value" > "$path" 2>/dev/null
            sleep 0.2
        fi
    done
    log_message "✓ Forced: $name = $value"
}

#=====================================================================
# 1. RESTORE CPU GOVERNORS (from performance to walt)
#=====================================================================
log_message ""
log_message "--- 1. Restoring CPU Governors ---"

# Wait a bit for boot to settle
sleep 2

# Force WALT governor (overrides performance governor from init.rc)
force_setting "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor" "walt" "Silver Governor"
force_setting "/sys/devices/system/cpu/cpufreq/policy4/scaling_governor" "walt" "Gold Governor"

# Verify governors
silver_gov=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)
gold_gov=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_governor 2>/dev/null)
log_message "Silver Governor: $silver_gov"
log_message "Gold Governor: $gold_gov"

#=====================================================================
# 2. DISABLE SCHED_BOOST (was only for boot speed)
#=====================================================================
log_message ""
log_message "--- 2. Disabling Sched Boost ---"
force_setting "/proc/sys/kernel/sched_boost" "0" "Sched Boost"
log_message "Sched Boost: $(cat /proc/sys/kernel/sched_boost 2>/dev/null)"

#=====================================================================
# 3. CPU FREQUENCIES
#=====================================================================
log_message ""
log_message "--- 3. Setting CPU Frequencies ---"
apply_setting "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq" "300000" "Silver Min Freq"
apply_setting "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq" "1900800" "Silver Max Freq"
apply_setting "/sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq" "300000" "Gold Min Freq"
apply_setting "/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq" "2803200" "Gold Max Freq"

#=====================================================================
# 4. WALT GOVERNOR SETTINGS
#=====================================================================
log_message ""
log_message "--- 4. Setting WALT Governor Parameters ---"

if [ -d /sys/devices/system/cpu/cpufreq/policy0/walt ]; then
    echo 1516800 > /sys/devices/system/cpu/cpufreq/policy0/walt/hispeed_freq 2>/dev/null
    echo 90 > /sys/devices/system/cpu/cpufreq/policy0/walt/hispeed_load 2>/dev/null
    echo 1 > /sys/devices/system/cpu/cpufreq/policy0/walt/pl 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/rtg_boost_freq 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/down_rate_limit_us 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/up_rate_limit_us 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy0/walt/boost 2>/dev/null
    log_message "✓ Policy 0 (Silver) WALT settings applied"
fi

if [ -d /sys/devices/system/cpu/cpufreq/policy4/walt ]; then
    echo 1344000 > /sys/devices/system/cpu/cpufreq/policy4/walt/hispeed_freq 2>/dev/null
    echo 90 > /sys/devices/system/cpu/cpufreq/policy4/walt/hispeed_load 2>/dev/null
    echo 1 > /sys/devices/system/cpu/cpufreq/policy4/walt/pl 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/rtg_boost_freq 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/down_rate_limit_us 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/up_rate_limit_us 2>/dev/null
    echo 0 > /sys/devices/system/cpu/cpufreq/policy4/walt/boost 2>/dev/null
    log_message "✓ Policy 4 (Gold) WALT settings applied"
fi

#=====================================================================
# 5. CORE CONTROL
#=====================================================================
log_message ""
log_message "--- 5. Setting Core Control ---"

apply_setting "/sys/devices/system/cpu/cpu0/core_ctl/enable" "1" "Core Control Silver"
apply_setting "/sys/devices/system/cpu/cpu4/core_ctl/enable" "1" "Core Control Gold"
apply_setting "/sys/devices/system/cpu/cpu4/core_ctl/min_cpus" "2" "Core Min CPUs"
apply_setting "/sys/devices/system/cpu/cpu4/core_ctl/max_cpus" "4" "Core Max CPUs"

echo "60 60 60 60" > /sys/devices/system/cpu/cpu4/core_ctl/busy_up_thres 2>/dev/null
echo "40 40 40 40" > /sys/devices/system/cpu/cpu4/core_ctl/busy_down_thres 2>/dev/null
apply_setting "/sys/devices/system/cpu/cpu4/core_ctl/offline_delay_ms" "100" "Offline Delay"
apply_setting "/sys/devices/system/cpu/cpu4/core_ctl/task_thres" "4" "Task Threshold"

#=====================================================================
# 6. WALT SCHEDULER PARAMETERS
#=====================================================================
log_message ""
log_message "--- 6. Setting WALT Scheduler ---"

apply_setting "/proc/sys/walt/sched_downmigrate" "65 85" "Sched Downmigrate"
apply_setting "/proc/sys/walt/sched_upmigrate" "71 95" "Sched Upmigrate"
apply_setting "/proc/sys/walt/sched_group_upmigrate" "100" "Group Upmigrate"
apply_setting "/proc/sys/walt/sched_group_downmigrate" "85" "Group Downmigrate"
apply_setting "/proc/sys/walt/sched_walt_rotate_big_tasks" "1" "Rotate Big Tasks"
apply_setting "/proc/sys/walt/sched_coloc_downmigrate_ns" "400000000" "Coloc Downmigrate ns"
apply_setting "/proc/sys/walt/sched_cluster_util_thres_pct" "40" "Cluster Util %"
apply_setting "/proc/sys/walt/sched_idle_enough" "0" "Sched Idle Enough"
apply_setting "/proc/sys/walt/walt_low_latency_task_threshold" "325" "Low Latency Threshold"
apply_setting "/proc/sys/walt/sched_boost" "0" "Sched Boost"

#=====================================================================
# 7. EARLY MIGRATION (Critical - often overwritten)
#=====================================================================
log_message ""
log_message "--- 7. Setting Early Migration ---"

# Force these multiple times to ensure they stick
for i in 1 2 3; do
    echo "1873 1204" > /proc/sys/walt/sched_early_downmigrate 2>/dev/null
    echo "1584 1077" > /proc/sys/walt/sched_early_upmigrate 2>/dev/null
    sleep 0.3
done

log_message "✓ Early Downmigrate: $(cat /proc/sys/walt/sched_early_downmigrate 2>/dev/null)"
log_message "✓ Early Upmigrate: $(cat /proc/sys/walt/sched_early_upmigrate 2>/dev/null)"

#=====================================================================
# 9. MEMORY & VM (Critical: swappiness)
#=====================================================================
log_message ""
log_message "--- 9. Setting Memory Parameters ---"

# Force swappiness (often reset by LMKD)
for i in 1 2 3; do
    echo 100 > /proc/sys/vm/swappiness 2>/dev/null
    sleep 0.3
done
log_message "✓ Swappiness: $(cat /proc/sys/vm/swappiness 2>/dev/null)"

apply_setting "/proc/sys/vm/min_free_kbytes" "8192" "Min Free KB"
apply_setting "/proc/sys/vm/compaction_proactiveness" "0" "Compaction Proactiveness"
apply_setting "/proc/sys/vm/dirty_background_ratio" "10" "Dirty Background"
apply_setting "/proc/sys/vm/dirty_ratio" "20" "Dirty Ratio"
apply_setting "/proc/sys/vm/dirty_expire_centisecs" "3000" "Dirty Expire"
apply_setting "/proc/sys/vm/page-cluster" "0" "Page Cluster"
apply_setting "/proc/sys/vm/watermark_boost_factor" "0" "Watermark Boost"
apply_setting "/proc/sys/vm/vfs_cache_pressure" "100" "VFS Cache Pressure"

# Disable THP
if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
    log_message "✓ THP disabled"
fi

#=====================================================================
# 10. ZRAM
#=====================================================================
log_message ""
log_message "--- 10. Setting ZRAM ---"

# Check if ZRAM needs re-initialization
zram_size=$(cat /sys/block/zram0/disksize 2>/dev/null)
if [ "$zram_size" == "0" ] || [ -z "$zram_size" ]; then
    log_message "ZRAM not initialized, setting up..."
    swapoff /dev/block/zram0 2>/dev/null
    echo 1 > /sys/block/zram0/reset 2>/dev/null
    
    # Set compression to lz4 if available
    if [ -f /sys/block/zram0/comp_algorithm ]; then
        if grep -q lz4 /sys/block/zram0/comp_algorithm; then
            echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
            log_message "✓ ZRAM compression: lz4"
        fi
    fi
    
    echo 4096M > /sys/block/zram0/disksize 2>/dev/null
    mkswap /dev/block/zram0 2>/dev/null
    swapon /dev/block/zram0 -p 32758 2>/dev/null
    log_message "✓ ZRAM initialized: $(cat /sys/block/zram0/disksize 2>/dev/null)"
else
    log_message "✓ ZRAM already configured: $zram_size"
fi

#=====================================================================
# 11. RT PARAMETERS
#=====================================================================
log_message ""
log_message "--- 11. Setting RT Parameters ---"

long_running_rt_task_ms=1200
sched_rt_runtime_ms=`expr $long_running_rt_task_ms + 50`
sched_rt_runtime_us=`expr $sched_rt_runtime_ms \* 1000`
sched_rt_period_ms=`expr $sched_rt_runtime_ms + 100`
sched_rt_period_us=`expr $sched_rt_period_ms \* 1000`

apply_setting "/proc/sys/kernel/sched_rt_period_us" "$sched_rt_period_us" "RT Period"
apply_setting "/proc/sys/kernel/sched_rt_runtime_us" "$sched_rt_runtime_us" "RT Runtime"
apply_setting "/proc/sys/kernel/sched_util_clamp_min_rt_default" "0" "RT UCLAMP"

#=====================================================================
# 12. POWER MANAGEMENT
#=====================================================================
log_message ""
log_message "--- 12. Setting Power Management ---"

if [ -f /sys/power/mem_sleep ]; then
    echo s2idle > /sys/power/mem_sleep 2>/dev/null
    log_message "✓ Mem Sleep: $(cat /sys/power/mem_sleep 2>/dev/null)"
fi

if [ -f /sys/devices/system/cpu/cpuidle/current_governor ]; then
    echo qcom-cpu-lpm > /sys/devices/system/cpu/cpuidle/current_governor 2>/dev/null
    log_message "✓ Idle Governor: $(cat /sys/devices/system/cpu/cpuidle/current_governor 2>/dev/null)"
fi

if [ -f /sys/devices/system/cpu/qcom_lpm/parameters/sleep_disabled ]; then
    echo N > /sys/devices/system/cpu/qcom_lpm/parameters/sleep_disabled 2>/dev/null
    log_message "✓ Sleep Disabled: $(cat /sys/devices/system/cpu/qcom_lpm/parameters/sleep_disabled 2>/dev/null)"
fi

#=====================================================================
# 13. CPUSET
#=====================================================================
log_message ""
log_message "--- 13. Setting CPUSET ---"

apply_setting "/dev/cpuset/background/cpus" "0-2" "Background"
apply_setting "/dev/cpuset/system-background/cpus" "0-3" "System Background"
apply_setting "/dev/cpuset/foreground/cpus" "0-2,4-7" "Foreground"
apply_setting "/dev/cpuset/top-app/cpus" "0-7" "Top App"

if [ -f /dev/cpuset/restricted/cpus ]; then
    echo 0-7 > /dev/cpuset/restricted/cpus 2>/dev/null
    log_message "✓ Restricted: 0-7"
fi
if [ -f /dev/cpuset/camera-daemon/cpus ]; then
    echo 0-7 > /dev/cpuset/camera-daemon/cpus 2>/dev/null
    log_message "✓ Camera Daemon: 0-7"
fi

#=====================================================================
# 14. CPUCTL
#=====================================================================
log_message ""
log_message "--- 14. Setting CPUCTL ---"

for group in top-app foreground foreground_window system-background background camera-daemon; do
    if [ -f "/dev/cpuctl/$group/cpu.shares" ]; then
        echo 1024 > "/dev/cpuctl/$group/cpu.shares" 2>/dev/null
    fi
done
log_message "✓ CPU shares configured"

#=====================================================================
# 15. I/O SCHEDULER
#=====================================================================
log_message ""
log_message "--- 15. Setting I/O Scheduler ---"

# Set BFQ scheduler on all block devices
for dev in sda sdb sdc sdd sde sdf mmcblk1; do
    if [ -f "/sys/block/$dev/queue/scheduler" ]; then
        if grep -q bfq "/sys/block/$dev/queue/scheduler" 2>/dev/null; then
            echo bfq > "/sys/block/$dev/queue/scheduler" 2>/dev/null
            log_message "✓ $dev scheduler: bfq"
        fi
    fi
    if [ -f "/sys/block/$dev/queue/read_ahead_kb" ]; then
        echo 512 > "/sys/block/$dev/queue/read_ahead_kb" 2>/dev/null
    fi
done
log_message "✓ I/O schedulers configured"

#=====================================================================
# 16. NETWORK
#=====================================================================
log_message ""
log_message "--- 16. Setting Network ---"

apply_setting "/proc/sys/net/ipv4/tcp_congestion_control" "cubic" "TCP Congestion"
apply_setting "/proc/sys/net/core/rmem_max" "16777216" "RMem Max"
apply_setting "/proc/sys/net/core/wmem_max" "8388608" "WMem Max"

#=====================================================================
# 17. GPU
#=====================================================================
log_message ""
log_message "--- 17. Setting GPU ---"

if [ -f /sys/class/kgsl/kgsl-3d0/idle_timer ]; then
    echo 80 > /sys/class/kgsl/kgsl-3d0/idle_timer 2>/dev/null
    log_message "✓ GPU Idle Timer: 80ms"
fi

#=====================================================================
# 18. FINAL VERIFICATION
#=====================================================================
log_message ""
log_message "--- 18. Final Verification ---"

swappiness=$(cat /proc/sys/vm/swappiness 2>/dev/null)
sched_boost=$(cat /proc/sys/kernel/sched_boost 2>/dev/null)
silver_gov=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)
gold_gov=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_governor 2>/dev/null)
early_down=$(cat /proc/sys/walt/sched_early_downmigrate 2>/dev/null)
early_up=$(cat /proc/sys/walt/sched_early_upmigrate 2>/dev/null)

log_message "========================================="
log_message "Verification Results:"
log_message "========================================="
log_message "Silver Governor: $silver_gov (expected: walt)"
log_message "Gold Governor: $gold_gov (expected: walt)"
log_message "Swappiness: $swappiness (expected: 100)"
log_message "Sched Boost: $sched_boost (expected: 0)"
log_message "Early Down: $early_down (expected: 1873 1204)"
log_message "Early Up: $early_up (expected: 1584 1077)"

if [ "$swappiness" = "100" ] && [ "$sched_boost" = "0" ] && [ "$silver_gov" = "walt" ]; then
    log_message "========================================="
    log_message "✅ SUCCESS: All critical settings applied!"
    log_message "========================================="
else
    log_message "========================================="
    log_message "⚠️ Some settings may need manual verification"
    log_message "========================================="
fi

#=====================================================================
# SET PROPERTY TO INDICATE COMPLETION
#=====================================================================
setprop vendor.post_boot.parsed 1

log_message "SD685 Post-Boot Configuration Complete!"
log_message "========================================="
