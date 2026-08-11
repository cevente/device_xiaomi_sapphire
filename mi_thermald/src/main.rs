//! Unified Thermal & Charging Daemon for Xiaomi Sapphire (sapphire).
//! Optimized Screen-ON Charge Control Curve for high speed <38.0°C with dynamic step-down to a strict 42.0°C cap.

#![allow(missing_docs)]
#![allow(clippy::needless_range_loop)]
#![allow(clippy::collapsible_else_if)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

// ── Sensor paths ────────────────────────────────────────────────────────────
const TZ_PA: &str = "/sys/class/thermal/thermal_zone18/temp";
const TZ_QUIET: &str = "/sys/class/thermal/thermal_zone19/temp";
const TZ_CHARGE: &str = "/sys/class/thermal/thermal_zone20/temp";
const TZ_EMMC: &str = "/sys/class/thermal/thermal_zone21/temp";
const TZ_BATTERY: &str = "/sys/class/thermal/thermal_zone34/temp";
const BAT_SOC_PATH: &str = "/sys/class/power_supply/battery/capacity";
const SCREEN_STATE_NODE: &str = "/sys/class/thermal/thermal_message/screen_state";

// ── Thermal control paths ───────────────────────────────────────────────────
const CPU0_MAX_FREQ: &str = "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq";
const CPU4_MAX_FREQ: &str = "/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq";
const GPU_MAX_FREQ: &str = "/sys/class/kgsl/kgsl-3d0/max_gpuclk";
const CPU2_ONLINE: &str = "/sys/devices/system/cpu/cpu2/online";
const CPU3_ONLINE: &str = "/sys/devices/system/cpu/cpu3/online";
const CPU6_ONLINE: &str = "/sys/devices/system/cpu/cpu6/online";

// Direct hardware brightness node (11-bit OLED, max 2047)
const BACKLIGHT_PATH: &str = "/sys/class/backlight/panel0-backlight/brightness";
const BL_LIMIT: i32 = 1280; // ~62% brightness threshold

const TEMP_STATE_PATH: &str = "/sys/class/thermal/thermal_message/temp_state";
const WIFI_LIMIT_PATH: &str = "/sys/class/thermal/thermal_message/wifi_limit";

// ── WALT Input Boost paths ──────────────────────────────────────────────────
const WALT_BOOST_FREQ_PATH: &str = "/proc/sys/walt/input_boost/input_boost_freq";
const WALT_BOOST_MS_PATH: &str = "/proc/sys/walt/input_boost/input_boost_ms";

// ── Charging control paths ──────────────────────────────────────────────────
const CHARGE_LIMIT_NODE: &str = "/sys/class/power_supply/battery/charge_control_limit";
const INPUT_SUSPEND_NODE: &str = "/sys/class/qcom-battery/input_suspend";
const FASTCHARGE_MODE_NODE: &str = "/sys/class/qcom-battery/fastcharge_mode";
const QUICK_CHARGE_TYPE_NODE: &str = "/sys/class/qcom-battery/quick_charge_type";

// ── DSP & Audio paths ───────────────────────────────────────────────────────
const TZ_CDSP_HVX: &str = "/sys/class/thermal/thermal_zone2/temp";
const CDSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device30/cur_state";
const ADSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device37/cur_state";
const USB_DAC_PATH: &str = "/sys/class/sound/card1";

// ── Virtual sensor parameters ───────────────────────────────────────────────
const WEIGHT_QUIET: i32 = 884;
const WEIGHT_PA: i32 = 75;
const WEIGHT_CHARGE: i32 = 28;
const WEIGHT_EMMC: i32 = 13;
const WEIGHT_BATTERY: i32 = 0;
const WEIGHT_SUM: i32 = 1000;
const COMPENSATION: i32 = 0;

// ── Thermal stepwise configurations ─────────────────────────────────────────
const CPU0_TRIG: [i32; 4] = [39000, 41000, 45000, 46000];
const CPU0_CLR: [i32; 4] = [37000, 39000, 44000, 45000];
const CPU0_TARGET: [i32; 4] = [1804800, 1516800, 1190400, 691200];
const CPU0_DEFAULT: i32 = 1900800;

const CPU4_TRIG: [i32; 6] = [33000, 35000, 38000, 42000, 44500, 45500];
const CPU4_CLR: [i32; 6] = [31000, 33000, 36000, 40000, 43000, 44500];
const CPU4_TARGET: [i32; 6] = [2592000, 2400000, 2208000, 1766400, 1344000, 806400];
const CPU4_DEFAULT: i32 = 2803200;

const GPU_TRIG: [i32; 3] = [43000, 45000, 46000];
const GPU_CLR: [i32; 3] = [41000, 43000, 45000];
const GPU_FREQS: [i32; 7] = [1260000000, 1114800000, 1025000000, 785000000, 600000000, 465000000, 320000000];
const GPU_TARGET_INDICES: [usize; 3] = [2, 4, 5];

const TSTATE_TRIG: [i32; 4] = [46000, 48000, 51000, 53000];
const TSTATE_CLR: [i32; 4] = [44000, 46000, 50000, 51000];
const TSTATE_TARGET: [i32; 4] = [110100000, 110100004, 112300001, 112520001];

// ── WALT String Configurations ──────────────────────────────────────────────
const BOOST_ENABLED_STR: &str = "1516800 0 0 0 1344000 0 0 0";
const BOOST_DISABLED_STR: &str = "0 0 0 0 0 0 0 0";

// ── Predictive control constants ──────────────────────────────────────────
const THERMAL_SPIKE_THRESHOLD_NORMALIZED: i32 = 750; // 0.75°C/s
const PROACTIVE_CPU_TEMP_BOOST: i32 = 3000;
const PROACTIVE_BOOST_TEMP_THRESHOLD: i32 = 42000;
const PROACTIVE_CHG_TEMP_THRESHOLD: i32 = 38500;

// ── Sleep duration constants ──────────────────────────────────────────────
const SCREEN_OFF_SLEEP: u64 = 8;
const NORMAL_SLEEP: u64 = 3;
const HOT_SLEEP: u64 = 1;

// ── High-Performance I/O Wrapper ────────────────────────────────────────────
struct SysfsNode {
    file: std::fs::File,
    path: &'static str,
    read_buf: String,
}

impl SysfsNode {
    fn new(path: &'static str, read_only: bool) -> Option<Self> {
        match OpenOptions::new().read(true).write(!read_only).open(path) {
            Ok(file) => Some(Self { file, path, read_buf: String::with_capacity(32) }),
            Err(e) => {
                eprintln!("Warning: Could not open {} ({})", path, e);
                None
            }
        }
    }

    fn read(&mut self) -> Option<i32> {
        self.file.seek(SeekFrom::Start(0)).ok()?;
        self.read_buf.clear();
        self.file.read_to_string(&mut self.read_buf).ok()?;
        self.read_buf.trim().parse().ok()
    }

    fn write(&mut self, value: i32) {
        if let Err(e) = self.file.seek(SeekFrom::Start(0)) {
            eprintln!("Failed to seek {}: {}", self.path, e);
            return;
        }
        let _ = self.file.set_len(0);

        let mut buf = [0u8; 32];
        let s = format!("{}\n", value);
        let bytes = s.as_bytes();
        let len = bytes.len().min(buf.len());
        buf[..len].copy_from_slice(&bytes[..len]);

        if let Err(e) = self.file.write_all(&buf[..len]) {
            eprintln!("Failed to write {} to {}: {}", value, self.path, e);
        }
    }

    fn write_str(&mut self, value: &str) {
        if let Err(e) = self.file.seek(SeekFrom::Start(0)) {
            eprintln!("Failed to seek {}: {}", self.path, e);
            return;
        }
        let _ = self.file.set_len(0);

        if let Err(e) = self.file.write_all(value.as_bytes()) {
            eprintln!("Failed to write string '{}' to {}: {}", value, self.path, e);
        }
    }
}

macro_rules! write_opt {
    ($node:expr, $value:expr) => {
        if let Some(n) = &mut $node { n.write($value); }
    };
}

macro_rules! write_str_opt {
    ($node:expr, $value:expr) => {
        if let Some(n) = &mut $node { n.write_str($value); }
    };
}

// ── Main ────────────────────────────────────────────────────────────────────
fn main() {
    println!("============================================================");
    println!(" Unified Thermal & Charging Daemon (Rust)");
    println!(" Dynamic Screen-ON Fast Charging Profile | Strict 42.0°C Cap");
    println!("============================================================");

    let mut node_t_pa = SysfsNode::new(TZ_PA, true);
    let mut node_t_quiet = SysfsNode::new(TZ_QUIET, true);
    let mut node_t_charge = SysfsNode::new(TZ_CHARGE, true);
    let mut node_t_emmc = SysfsNode::new(TZ_EMMC, true);
    let mut node_t_battery = SysfsNode::new(TZ_BATTERY, true);
    let mut node_soc = SysfsNode::new(BAT_SOC_PATH, true);
    let mut node_screen = SysfsNode::new(SCREEN_STATE_NODE, true);

    let mut node_cpu0 = SysfsNode::new(CPU0_MAX_FREQ, false);
    let mut node_cpu4 = SysfsNode::new(CPU4_MAX_FREQ, false);
    let mut node_gpu = SysfsNode::new(GPU_MAX_FREQ, false);
    let mut node_cpu2_on = SysfsNode::new(CPU2_ONLINE, false);
    let mut node_cpu3_on = SysfsNode::new(CPU3_ONLINE, false);
    let mut node_cpu6_on = SysfsNode::new(CPU6_ONLINE, false);
    let mut node_backlight = SysfsNode::new(BACKLIGHT_PATH, false);
    let mut node_tstate = SysfsNode::new(TEMP_STATE_PATH, false);
    let mut node_wifi = SysfsNode::new(WIFI_LIMIT_PATH, false);
    let mut node_chg_limit = SysfsNode::new(CHARGE_LIMIT_NODE, false);
    let mut node_input_suspend = SysfsNode::new(INPUT_SUSPEND_NODE, false);
    let mut node_fastcharge_mode = SysfsNode::new(FASTCHARGE_MODE_NODE, true);
    let mut node_quick_charge_type = SysfsNode::new(QUICK_CHARGE_TYPE_NODE, true);

    let mut node_walt_boost = SysfsNode::new(WALT_BOOST_FREQ_PATH, false);
    let mut node_walt_ms = SysfsNode::new(WALT_BOOST_MS_PATH, false);

    let mut node_t_hvx = SysfsNode::new(TZ_CDSP_HVX, true);
    let mut node_cdsp = SysfsNode::new(CDSP_CUR_STATE, false);
    let mut node_adsp = SysfsNode::new(ADSP_CUR_STATE, false);

    write_opt!(node_chg_limit, 0);
    write_opt!(node_input_suspend, 0);

    write_str_opt!(node_walt_ms, "80");
    write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);

    let mut prev_virtual_temp: Option<i32> = None;
    let mut state_cpu0: usize = 0;
    let mut state_cpu4: usize = 0;
    let mut state_gpu: usize = 0;
    let mut state_tstate: usize = 0;
    let mut state_backlight_clamped: bool = false;
    let mut state_wifi: i32 = 0;
    let mut state_boost: i32 = 1;
    let mut state_ccc_hotplug: bool = false;
    let mut state_bcl_hotplug: bool = false;

    let mut charge_paused_at_full: bool = false;
    let mut full_charge_start_time: Option<Instant> = None;

    let mut state_cdsp: i32 = 0;
    let mut state_adsp: i32 = 0;

    let mut last_update = Instant::now();

    loop {
        let t_pa = node_t_pa.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let t_quiet = node_t_quiet.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let t_charge = node_t_charge.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let t_emmc = node_t_emmc.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let t_battery = node_t_battery.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let soc = node_soc.as_mut().and_then(|n| n.read()).unwrap_or(100);
        let screen_state = node_screen.as_mut().and_then(|n| n.read()).unwrap_or(0);

        let fastcharge_mode = node_fastcharge_mode.as_mut().and_then(|n| n.read()).unwrap_or(-1);
        let quick_charge_type = node_quick_charge_type.as_mut().and_then(|n| n.read()).unwrap_or(-1);

        let virtual_temp = (WEIGHT_QUIET * t_quiet
            + WEIGHT_PA * t_pa
            + WEIGHT_CHARGE * t_charge
            + WEIGHT_EMMC * t_emmc
            + WEIGHT_BATTERY * t_battery)
            / WEIGHT_SUM
            + COMPENSATION;
        let virtual_c = virtual_temp / 1000;
        let batt_temp = t_battery / 1000;

        let now = Instant::now();
        let elapsed_secs = now.duration_since(last_update).as_secs_f64();
        last_update = now;

        let (temp_delta_normalized, is_thermal_spike) = match prev_virtual_temp {
            Some(prev) => {
                let raw_delta = virtual_temp - prev;
                let normalized = if elapsed_secs > 0.0 { (raw_delta as f64 / elapsed_secs) as i32 } else { 0 };
                (normalized, normalized >= THERMAL_SPIKE_THRESHOLD_NORMALIZED)
            }
            None => (0, false),
        };
        prev_virtual_temp = Some(virtual_temp);

        let virtual_temp_for_cpu = if is_thermal_spike && virtual_temp >= 35000 {
            virtual_temp + PROACTIVE_CPU_TEMP_BOOST
        } else {
            virtual_temp
        };

        // CPU0
        for i in (0..4).rev() {
            if virtual_temp_for_cpu >= CPU0_TRIG[i] && state_cpu0 <= i {
                state_cpu0 = i + 1;
                write_opt!(node_cpu0, CPU0_TARGET[i]);
                break;
            }
        }
        for i in 0..4 {
            if virtual_temp_for_cpu <= CPU0_CLR[i] && state_cpu0 > i {
                state_cpu0 = i;
                let val = if i == 0 { CPU0_DEFAULT } else { CPU0_TARGET[i - 1] };
                write_opt!(node_cpu0, val);
                break;
            }
        }

        // CPU4
        for i in (0..6).rev() {
            if virtual_temp_for_cpu >= CPU4_TRIG[i] && state_cpu4 <= i {
                state_cpu4 = i + 1;
                write_opt!(node_cpu4, CPU4_TARGET[i]);
                break;
            }
        }
        for i in 0..6 {
            if virtual_temp_for_cpu <= CPU4_CLR[i] && state_cpu4 > i {
                state_cpu4 = i;
                let val = if i == 0 { CPU4_DEFAULT } else { CPU4_TARGET[i - 1] };
                write_opt!(node_cpu4, val);
                break;
            }
        }

        // GPU
        let mut new_gpu = state_gpu;
        for i in (0..3).rev() {
            if virtual_temp >= GPU_TRIG[i] && state_gpu <= i {
                new_gpu = i + 1;
                break;
            }
        }
        if new_gpu <= state_gpu {
            for i in 0..3 {
                if virtual_temp <= GPU_CLR[i] && state_gpu > i {
                    new_gpu = i;
                    break;
                }
            }
        }
        if new_gpu != state_gpu {
            state_gpu = new_gpu;
            let val = if state_gpu == 0 { GPU_FREQS[0] } else { GPU_FREQS[GPU_TARGET_INDICES[state_gpu - 1]] };
            write_opt!(node_gpu, val);
        }

        // DSP & Audio
        let t_hvx = node_t_hvx.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let dac_connected = Path::new(USB_DAC_PATH).exists();

        if dac_connected {
            if state_adsp != 0 {
                state_adsp = 0;
                write_opt!(node_adsp, 0);
            }
        } else if virtual_temp >= 46000 && state_adsp == 0 {
            state_adsp = 1;
            write_opt!(node_adsp, 1);
        } else if virtual_temp <= 43000 && state_adsp == 1 {
            state_adsp = 0;
            write_opt!(node_adsp, 0);
        }

        let mut new_cdsp = state_cdsp;
        if t_hvx >= 52000 { new_cdsp = 5; }
        else if t_hvx >= 50000 { new_cdsp = 4; }
        else if t_hvx >= 48000 { new_cdsp = 3; }
        else if t_hvx >= 46000 { new_cdsp = 2; }
        else if t_hvx >= 44000 { new_cdsp = 1; }
        else if t_hvx <= 42000 { new_cdsp = 0; }

        if new_cdsp != state_cdsp {
            state_cdsp = new_cdsp;
            write_opt!(node_cdsp, state_cdsp);
        }

        // Backlight Clamp
        if virtual_temp >= 51000 {
            let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);
            if current_bl > BL_LIMIT {
                write_opt!(node_backlight, BL_LIMIT);
                state_backlight_clamped = true;
            }
        } else if virtual_temp <= 49000 && state_backlight_clamped {
            state_backlight_clamped = false;
        }

        // Hotplug
        if virtual_temp >= 49000 && !state_ccc_hotplug { state_ccc_hotplug = true; }
        else if virtual_temp <= 47000 && state_ccc_hotplug { state_ccc_hotplug = false; }

        if soc <= 5 && !state_bcl_hotplug { state_bcl_hotplug = true; }
        else if soc >= 6 && state_bcl_hotplug { state_bcl_hotplug = false; }

        let offline = state_ccc_hotplug || state_bcl_hotplug;
        let core_val = if offline { 0 } else { 1 };
        write_opt!(node_cpu2_on, core_val);
        write_opt!(node_cpu3_on, core_val);
        write_opt!(node_cpu6_on, core_val);

        // Smart Idle Charge Control
        if soc >= 100 && screen_state == 0 && !charge_paused_at_full {
            if let Some(start_time) = full_charge_start_time {
                if start_time.elapsed().as_secs() >= 600 {
                    write_opt!(node_input_suspend, 1);
                    charge_paused_at_full = true;
                    full_charge_start_time = None;
                }
            } else {
                full_charge_start_time = Some(Instant::now());
            }
        } else {
            full_charge_start_time = None;
            if charge_paused_at_full && screen_state == 1 {
                write_opt!(node_input_suspend, 0);
                charge_paused_at_full = false;
            }
        }

        // ── CHARGE CONTROL LIMIT ALGORITHM ─────────────────────────────────
        let mut target_chg_limit: i32;

        if charge_paused_at_full {
            target_chg_limit = 18;
        } else {
            if screen_state == 1 {
                // ── SCREEN ON: Fast-Dynamic Profile
                // Maximizes charge current when battery is cool (<38.0°C),
                // smoothly stepping down as thermals approach the 42.0°C cap.
                if t_battery >= 41800 || virtual_temp >= 47500 {
                    target_chg_limit = 18; // Strict 42.0°C Hard Thermal Cap
                } else if t_battery >= 40800 || virtual_temp >= 46000 {
                    target_chg_limit = 14; // Pre-cap buffer (~1.0A)
                } else if t_battery >= 39500 || virtual_temp >= 44000 {
                    target_chg_limit = 9;  // Moderate current (~1.8A)
                } else if t_battery >= 38000 || virtual_temp >= 42000 {
                    target_chg_limit = 5;  // Sustained fast current (~2.3A)
                } else {
                    // Cold/Cool (<38.0°C): Unlocks max screen-on speeds based on protocol
                    target_chg_limit = match (fastcharge_mode, quick_charge_type) {
                        (1, 3) => 2,  // Xiaomi HyperCharge: ~3.0A allowed when cool
                        (0, 2) => 5,  // QC 2.0: ~2.2A
                        (0, 0) => 4,  // Std 5V: ~2.4A
                        _      => 6,  // QC 1.0 / Fallback
                    };
                }

                // Apply thermal spike adjustment ONLY when warming up (>38.5°C)
                if is_thermal_spike && t_battery >= 38500 {
                    target_chg_limit = (target_chg_limit + 2).min(17);
                }

                // High brightness thermal stack-up check (only when battery >= 39.0°C)
                let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);
                if current_bl > BL_LIMIT && t_battery >= 39000 {
                    target_chg_limit = (target_chg_limit + 2).min(17);
                }
            } else {
                // ── SCREEN OFF: Fast speed profile with 42.0°C ceiling
                if t_battery >= 41800 || virtual_temp >= 47500 {
                    target_chg_limit = 18; // Strict 42.0°C hard thermal cap
                } else if t_battery >= 40500 || virtual_temp >= 45500 {
                    target_chg_limit = 15;
                } else if t_battery >= 39000 || virtual_temp >= 43500 {
                    target_chg_limit = 11;
                } else if t_battery >= 37500 || virtual_temp >= 41500 {
                    target_chg_limit = 7;
                } else if t_battery >= 36000 || virtual_temp >= 39500 {
                    target_chg_limit = 3;
                } else {
                    target_chg_limit = 0; // Unrestricted fast charging
                }

                if is_thermal_spike && batt_temp >= PROACTIVE_CHG_TEMP_THRESHOLD {
                    target_chg_limit = (target_chg_limit + 4).max(12);
                }
            }
        }

        let target_chg_limit = target_chg_limit.clamp(0, 18);
        write_opt!(node_chg_limit, target_chg_limit);

        println!(
            "V:{}°C | B:{}°C | Δ:{:+.1}°C/s | SOC:{}% | CHG_LIM:{} [FM:{} QC:{}] S:{}",
            virtual_c,
            batt_temp,
            temp_delta_normalized as f32 / 1000.0,
            soc,
            target_chg_limit,
            fastcharge_mode,
            quick_charge_type,
            screen_state
        );

        let sleep_duration = match screen_state {
            0 => Duration::from_secs(SCREEN_OFF_SLEEP),
            _ if virtual_temp >= 42000 || is_thermal_spike => Duration::from_secs(HOT_SLEEP),
            _ => Duration::from_secs(NORMAL_SLEEP),
        };

        thread::sleep(sleep_duration);
    }
}
