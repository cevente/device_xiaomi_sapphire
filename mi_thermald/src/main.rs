//! Unified Thermal & Charging Daemon for Xiaomi Sapphire (sapphire).
//! Optimized charging daemon supporting all charger protocols (Xiaomi HyperCharge, QC2.0, QC1.0, Std 5V).
//! - Maximized Screen-OFF charging speeds up to a strict 42.0°C hard thermal limit (+2°C total adjustment).
//! - Increased Screen-ON charging thermal thresholds by an additional +1.5°C for higher active current retention.
//! - Fixed virtual sensor weights to match OEM configuration (negative weights for localized sensors)

#![allow(missing_docs)]
#![allow(clippy::needless_range_loop)]
#![allow(clippy::collapsible_else_if)]
#![allow(unused_variables)]
#![allow(unused_assignments)]

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// ── Sensor paths ────────────────────────────────────────────────────────────
const TZ_PA: &str = "/sys/class/thermal/thermal_zone18/temp";
const TZ_QUIET: &str = "/sys/class/thermal/thermal_zone19/temp";
const TZ_CHARGE: &str = "/sys/class/thermal/thermal_zone20/temp";
const TZ_EMMC: &str = "/sys/class/thermal/thermal_zone21/temp";
const TZ_BATTERY: &str = "/sys/class/thermal/thermal_zone34/temp";
const BAT_SOC_PATH: &str = "/sys/class/power_supply/battery/capacity";
const SCREEN_STATE_NODE: &str = "/sys/class/thermal/thermal_message/screen_state";
const USB_ONLINE_NODE: &str = "/sys/class/power_supply/usb/online";

// ── Thermal control paths ───────────────────────────────────────────────────
const CPU0_MAX_FREQ: &str = "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq";
const CPU4_MAX_FREQ: &str = "/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq";
const GPU_MAX_FREQ: &str = "/sys/class/kgsl/kgsl-3d0/max_gpuclk";
const CPU2_ONLINE: &str = "/sys/devices/system/cpu/cpu2/online";
const CPU3_ONLINE: &str = "/sys/devices/system/cpu/cpu3/online";
const CPU6_ONLINE: &str = "/sys/devices/system/cpu/cpu6/online";

// Direct hardware brightness node (11-bit OLED, max 2047)
const BACKLIGHT_PATH: &str = "/sys/class/backlight/panel0-backlight/brightness";
const BL_LIMIT: i32 = 1280; // ~62% of 2047 - thermal throttle threshold

const TEMP_STATE_PATH: &str = "/sys/class/thermal/thermal_message/temp_state";
const WIFI_LIMIT_PATH: &str = "/sys/class/thermal/thermal_message/wifi_limit";

// ── WALT Input Boost paths ──────────────────────────────────────────────────
const WALT_BOOST_FREQ_PATH: &str = "/proc/sys/walt/input_boost/input_boost_freq";
const WALT_BOOST_MS_PATH: &str = "/proc/sys/walt/input_boost/input_boost_ms";

// ── Charging control paths ──────────────────────────────────────────────────
const CHARGE_LIMIT_NODE: &str = "/sys/class/power_supply/battery/charge_control_limit";
const RESTRICT_CHG_NODE: &str = "/sys/class/qcom-battery/restrict_chg";
const RESTRICT_CUR_NODE: &str = "/sys/class/qcom-battery/restrict_cur";
const INPUT_SUSPEND_NODE: &str = "/sys/class/qcom-battery/input_suspend"; // Smart Idle Charge Control
const FASTCHARGE_MODE_NODE: &str = "/sys/class/qcom-battery/fastcharge_mode";
const QUICK_CHARGE_TYPE_NODE: &str = "/sys/class/qcom-battery/quick_charge_type";

// ── DSP & Audio paths ───────────────────────────────────────────────────────
const TZ_CDSP_HVX: &str = "/sys/class/thermal/thermal_zone2/temp";
const CDSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device30/cur_state";
const ADSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device37/cur_state";
const USB_DAC_PATH: &str = "/sys/class/sound/card1";

// ── Virtual sensor parameters (FROM ORIGINAL OEM CONFIG) ──────────────────
// These match the OEM mi_thermald configuration exactly
const WEIGHT_QUIET: i32 = 1000;
const WEIGHT_PA: i32 = -224;    // Negative! PA is a localized hotspot
const WEIGHT_CHARGE: i32 = -12;  // Negative! Charge IC is localized
const WEIGHT_EMMC: i32 = 92;
const WEIGHT_BATTERY: i32 = 203;
const WEIGHT_SUM: i32 = 1000;
const COMPENSATION: i32 = -2893; // -2.893°C calibration offset

// ── Thermal stepwise/monitor configurations (FROM ORIGINAL OEM CONFIG) ────
const CPU0_TRIG: [i32; 4] = [37000, 39000, 41000, 42000];
const CPU0_CLR: [i32; 4] = [36000, 38000, 40000, 41000];
const CPU0_TARGET: [i32; 4] = [1804800, 1516800, 1190400, 691200];
const CPU0_DEFAULT: i32 = 1900800;

const CPU4_TRIG: [i32; 6] = [32000, 34000, 36000, 39000, 41000, 42000];
const CPU4_CLR: [i32; 6] = [31000, 33000, 35000, 38000, 40000, 41000];
const CPU4_TARGET: [i32; 6] = [2400000, 2208000, 1766400, 1344000, 1056000, 806400];
const CPU4_DEFAULT: i32 = 2803200;

const GPU_TRIG: [i32; 3] = [39000, 41000, 42000];  // FROM OEM CONFIG
const GPU_CLR: [i32; 3] = [38000, 40000, 41000];   // FROM OEM CONFIG
const GPU_FREQS: [i32; 7] = [1260000000, 1114800000, 1025000000, 785000000, 600000000, 465000000, 320000000];
const GPU_TARGET_INDICES: [usize; 3] = [2, 4, 5];   // Target indices for GPU levels

// MONITOR-TEMP_STATE from OEM config
const TSTATE_TRIG: [i32; 4] = [41000, 43000, 46000, 48000];
const TSTATE_CLR: [i32; 4] = [40000, 42000, 45000, 46000];
const TSTATE_TARGET: [i32; 4] = [110100000, 110100004, 112300001, 112520001];

// MONITOR-WIFI-LIMIT from OEM config
const WIFI_TRIG: i32 = 40000;
const WIFI_CLR: i32 = 38000;

// MONITOR-BACKLIGHT from OEM config
const BL_TRIG: i32 = 45000;
const BL_CLR: i32 = 43000;

// MONITOR-CCC from OEM config
const CCC_TRIG: i32 = 43000;
const CCC_CLR: i32 = 41000;

// MONITOR-BOOST_LIMIT from OEM config
const BOOST_TRIG: i32 = 42000;
const BOOST_CLR: i32 = 40000;

// ── WALT String Configurations ──────────────────────────────────────────────
const BOOST_ENABLED_STR: &str = "1190400 0 0 0 1113600 0 0 0";
const BOOST_DISABLED_STR: &str = "0 0 0 0 0 0 0 0";

// ── Predictive control constants ──────────────────────────────────────────
const THERMAL_SPIKE_THRESHOLD_NORMALIZED: i32 = 750;
const PROACTIVE_CPU_TEMP_BOOST: i32 = 3000;
const PROACTIVE_BOOST_TEMP_THRESHOLD: i32 = 34500;
const PROACTIVE_CHG_TEMP_THRESHOLD: i32 = 38500;

// ── Sleep duration constants ──────────────────────────────────────────────
const SCREEN_OFF_SLEEP: u64 = 8;
const NORMAL_SLEEP: u64 = 3;
const HOT_SLEEP: u64 = 1;

// ── Signal handling with direct FFI (zero external dependencies) ────────────
static RUNNING: AtomicBool = AtomicBool::new(true);

// Signal constants (Linux/Android)
const SIGINT: i32 = 2;
const SIGTERM: i32 = 15;

extern "C" fn handle_sig(_sig: i32) {
    RUNNING.store(false, Ordering::SeqCst);
}

// Direct FFI binding to system signal function
extern "C" {
    fn signal(sig: i32, handler: extern "C" fn(i32)) -> usize;
}

// ── High-Performance I/O Wrapper ────────────────────────────────────────────
struct SysfsNode {
    file: std::fs::File,
    path: &'static str,
    read_buf: String,
}

impl SysfsNode {
    fn new(path: &'static str, read_only: bool) -> Option<Self> {
        match OpenOptions::new().read(true).write(!read_only).open(path) {
            Ok(file) => Some(Self {
                file,
                path,
                read_buf: String::with_capacity(32),
            }),
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

// ── Helpers for optional writes ────────────────────────────────────────────
macro_rules! write_opt {
    ($node:expr, $value:expr) => {
        if let Some(n) = &mut $node {
            n.write($value);
        }
    };
}

macro_rules! write_str_opt {
    ($node:expr, $value:expr) => {
        if let Some(n) = &mut $node {
            n.write_str($value);
        }
    };
}

macro_rules! read_sensor_safe {
    ($node:expr, $last_val:expr) => {
        match $node.as_mut().and_then(|n| n.read()) {
            Some(val) if val > 0 && val < 80000 => {
                $last_val = val;
                val
            }
            _ => $last_val,
        }
    };
}

// ── Thermal State Logging (no chrono dependency) ──────────────────────────
fn get_timestamp() -> String {
    let now = SystemTime::now();
    let since_epoch = now.duration_since(UNIX_EPOCH).unwrap_or(Duration::from_secs(0));
    let secs = since_epoch.as_secs();
    
    let days = secs / 86400;
    let secs_rem = secs % 86400;
    let hours = secs_rem / 3600;
    let mins = (secs_rem % 3600) / 60;
    let secs_final = secs_rem % 60;
    
    // Simple format: "2024-01-15 14:30:45" based on UNIX epoch
    // Note: This doesn't account for timezone, but works for logging
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", 
        1970 + (days / 365) as u32,
        ((days % 365) / 30) as u32 + 1,
        (days % 30) as u32 + 1,
        hours as u32,
        mins as u32,
        secs_final as u32
    )
}

fn log_thermal_state(virtual_temp: i32, battery_temp: i32, soc: i32, current_limit: i32, 
                     usb_online: i32, screen_state: i32, state_cpu0: usize, state_cpu4: usize) {
    let timestamp = get_timestamp();
    let line = format!(
        "[{}] V:{}°C B:{}°C C0:L{} C4:L{} CHG:{}mA SOC:{}% USB:{} SCREEN:{}\n",
        timestamp,
        virtual_temp / 1000,
        battery_temp / 1000,
        state_cpu0,
        state_cpu4,
        current_limit,
        soc,
        usb_online,
        screen_state
    );
    
    let _ = OpenOptions::new()
        .append(true)
        .create(true)
        .open("/data/vendor/thermal/thermal.dump")
        .and_then(|mut f| f.write_all(line.as_bytes()));
}

// ── Main ────────────────────────────────────────────────────────────────────
fn main() {
    println!("============================================================");
    println!(" Unified Thermal & Charging Daemon (Rust)");
    println!(" OEM-Corrected Virtual Sensor Weights");
    println!(" Proactive Rate-of-Change Control");
    println!(" Audio-Aware CPU Floor | Graceful Restoration | Safe-Sensors");
    println!(" Smart Idle Charge Control | Screen-ON Limits Shifted +1.5°C");
    println!("============================================================");

    // ── Signal Handling (zero external dependencies) ─────────────────────
    // SAFETY: The signal function is a standard POSIX system call that is
    // safe to call in this context. The signal handlers are simple functions
    // that only set an atomic flag, which is safe to do in a signal context.
    unsafe {
        signal(SIGINT, handle_sig);
        signal(SIGTERM, handle_sig);
    }

    // ── Startup Delay ─────────────────────────────────────────────────────
    println!("[DAEMON] Waiting 5 seconds for system to stabilize...");
    thread::sleep(Duration::from_secs(5));

    // Initialise cached sensor nodes
    let mut node_t_pa = SysfsNode::new(TZ_PA, true);
    let mut node_t_quiet = SysfsNode::new(TZ_QUIET, true);
    let mut node_t_charge = SysfsNode::new(TZ_CHARGE, true);
    let mut node_t_emmc = SysfsNode::new(TZ_EMMC, true);
    let mut node_t_battery = SysfsNode::new(TZ_BATTERY, true);
    let mut node_soc = SysfsNode::new(BAT_SOC_PATH, true);
    let mut node_screen = SysfsNode::new(SCREEN_STATE_NODE, true);
    let mut node_usb_online = SysfsNode::new(USB_ONLINE_NODE, true);

    // Initialise cached control nodes
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
    let mut node_res_chg = SysfsNode::new(RESTRICT_CHG_NODE, false);
    let mut node_res_cur = SysfsNode::new(RESTRICT_CUR_NODE, false);
    let mut node_input_suspend = SysfsNode::new(INPUT_SUSPEND_NODE, false);
    let mut node_fastcharge_mode = SysfsNode::new(FASTCHARGE_MODE_NODE, true);
    let mut node_quick_charge_type = SysfsNode::new(QUICK_CHARGE_TYPE_NODE, true);

    // Initialise WALT nodes
    let mut node_walt_boost = SysfsNode::new(WALT_BOOST_FREQ_PATH, false);
    let mut node_walt_ms = SysfsNode::new(WALT_BOOST_MS_PATH, false);

    // Initialise DSP nodes
    let mut node_t_hvx = SysfsNode::new(TZ_CDSP_HVX, true);
    let mut node_cdsp = SysfsNode::new(CDSP_CUR_STATE, false);
    let mut node_adsp = SysfsNode::new(ADSP_CUR_STATE, false);

    // Initial charging state
    write_opt!(node_res_chg, 0);
    write_opt!(node_res_cur, 1500000);
    write_opt!(node_input_suspend, 0);

    write_str_opt!(node_walt_ms, "40");
    write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);

    // State tracking
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
    let mut restricted: bool = false;
    let mut prev_screen_state: i32 = 0;
    let mut charge_paused_at_full: bool = false;
    let mut full_charge_start_time: Option<Instant> = None;
    let mut state_cdsp: i32 = 0;
    let mut state_adsp: i32 = 0;
    let mut last_update = Instant::now();
    let mut last_log = Instant::now();

    // Baseline fallbacks
    let mut last_t_pa = 35000;
    let mut last_t_quiet = 35000;
    let mut last_t_charge = 35000;
    let mut last_t_emmc = 35000;
    let mut last_t_battery = 35000;
    let mut last_t_hvx = 35000;

    // ── Runtime loop ──────────────────────────────────────────────────────
    while RUNNING.load(Ordering::SeqCst) {
        // ── Read sensors with Failsafe Fallbacks ─────────────────────────
        let t_pa = read_sensor_safe!(node_t_pa, last_t_pa);
        let t_quiet = read_sensor_safe!(node_t_quiet, last_t_quiet);
        let t_charge = read_sensor_safe!(node_t_charge, last_t_charge);
        let t_emmc = read_sensor_safe!(node_t_emmc, last_t_emmc);
        let t_battery = read_sensor_safe!(node_t_battery, last_t_battery);
        let t_hvx = read_sensor_safe!(node_t_hvx, last_t_hvx);
        
        let soc = node_soc.as_mut().and_then(|n| n.read()).unwrap_or(100);
        let screen_state = node_screen.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let usb_online = node_usb_online.as_mut().and_then(|n| n.read()).unwrap_or(0);
        let dac_connected = Path::new(USB_DAC_PATH).exists();

        // Read Charger protocol nodes
        let fastcharge_mode = node_fastcharge_mode.as_mut().and_then(|n| n.read()).unwrap_or(-1);
        let quick_charge_type = node_quick_charge_type.as_mut().and_then(|n| n.read()).unwrap_or(-1);

        // ── Virtual temperature (OEM-CORRECTED WEIGHTS) ──────────────────
        // Using the exact weights from the original mi_thermald config
        let virtual_temp = (WEIGHT_QUIET * t_quiet
            + WEIGHT_PA * t_pa          // Negative! PA is a localized hotspot
            + WEIGHT_CHARGE * t_charge  // Negative! Charge IC is localized
            + WEIGHT_EMMC * t_emmc
            + WEIGHT_BATTERY * t_battery)
            / WEIGHT_SUM
            + COMPENSATION;
            
        let virtual_c = virtual_temp / 1000;
        let batt_temp = t_battery / 1000;

        // ── Calculate normalized thermal derivative ──────────────────────────
        let now = Instant::now();
        let elapsed_secs = now.duration_since(last_update).as_secs_f64();
        last_update = now;

        let (temp_delta_normalized, is_thermal_spike) = match prev_virtual_temp {
            Some(prev) => {
                let raw_delta = virtual_temp - prev;
                let normalized = if elapsed_secs > 0.0 {
                    (raw_delta as f64 / elapsed_secs) as i32
                } else {
                    0
                };
                let spike = normalized >= THERMAL_SPIKE_THRESHOLD_NORMALIZED;
                (normalized, spike)
            }
            None => (0, false),
        };
        prev_virtual_temp = Some(virtual_temp);

        if is_thermal_spike {
            println!("[PROACTIVE] Rapid thermal rise detected! Rate: +{:.1}°C/s", temp_delta_normalized as f32 / 1000.0);
        }

        let virtual_temp_for_cpu = if is_thermal_spike && virtual_temp >= 35000 {
            virtual_temp + PROACTIVE_CPU_TEMP_BOOST
        } else {
            virtual_temp
        };

        // ── CPU0 (Audio-Aware Floor) ─────────────────────────────────────
        let max_cpu0_idx = if dac_connected { 2 } else { 3 };

        if state_cpu0 > max_cpu0_idx + 1 {
            state_cpu0 = max_cpu0_idx + 1;
            let val = CPU0_TARGET[max_cpu0_idx];
            println!("[CPU0] DAC Connected! Forcing throttle lift to L{} ({}Hz)", state_cpu0, val);
            write_opt!(node_cpu0, val);
        }

        for i in (0..=max_cpu0_idx).rev() {
            if virtual_temp_for_cpu >= CPU0_TRIG[i] && state_cpu0 <= i {
                state_cpu0 = i + 1;
                println!("[CPU0] Up -> L{} ({}Hz) [Proactive]", state_cpu0, CPU0_TARGET[i]);
                write_opt!(node_cpu0, CPU0_TARGET[i]);
                break;
            }
        }
        for i in 0..=max_cpu0_idx {
            if virtual_temp_for_cpu <= CPU0_CLR[i] && state_cpu0 > i {
                state_cpu0 = i;
                let val = if i == 0 { CPU0_DEFAULT } else { CPU0_TARGET[i - 1] };
                println!("[CPU0] Down -> L{} ({}Hz)", state_cpu0, val);
                write_opt!(node_cpu0, val);
                break;
            }
        }

        // ── CPU4 ─────────────────────────────────────────────────────────
        for i in (0..6).rev() {
            if virtual_temp_for_cpu >= CPU4_TRIG[i] && state_cpu4 <= i {
                state_cpu4 = i + 1;
                println!("[CPU4] Up -> L{} ({}Hz) [Proactive]", state_cpu4, CPU4_TARGET[i]);
                write_opt!(node_cpu4, CPU4_TARGET[i]);
                break;
            }
        }
        for i in 0..6 {
            if virtual_temp_for_cpu <= CPU4_CLR[i] && state_cpu4 > i {
                state_cpu4 = i;
                let val = if i == 0 { CPU4_DEFAULT } else { CPU4_TARGET[i - 1] };
                println!("[CPU4] Down -> L{} ({}Hz)", state_cpu4, val);
                write_opt!(node_cpu4, val);
                break;
            }
        }

        // ── GPU (OEM thresholds) ─────────────────────────────────────────
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
            if state_gpu == 0 {
                println!("[GPU] Restored to {}Hz", GPU_FREQS[0]);
                write_opt!(node_gpu, GPU_FREQS[0]);
            } else {
                let freq = GPU_FREQS[GPU_TARGET_INDICES[state_gpu - 1]];
                println!("[GPU] Throttled L{} -> {}Hz", state_gpu, freq);
                write_opt!(node_gpu, freq);
            }
        }

        // ── ADSP (Audio) Control ──────────────────────────────────────
        if dac_connected {
            if state_adsp != 0 {
                state_adsp = 0;
                println!("[AUDIO] USB DAC Active: ADSP throttling locked to 0");
                write_opt!(node_adsp, 0);
            }
        } else if virtual_temp >= 46000 && state_adsp == 0 {
            state_adsp = 1;
            println!("[ADSP] Throttled (Level 1)");
            write_opt!(node_adsp, 1);
        } else if virtual_temp <= 43000 && state_adsp == 1 {
            state_adsp = 0;
            println!("[ADSP] Restored (Level 0)");
            write_opt!(node_adsp, 0);
        }

        // ── CDSP (Camera/Compute) Control ─────────────────────────────
        let mut new_cdsp = state_cdsp;

        if t_hvx >= 52000 {
            new_cdsp = 5;
        } else if t_hvx >= 50000 {
            new_cdsp = 4;
        } else if t_hvx >= 48000 {
            new_cdsp = 3;
        } else if t_hvx >= 46000 {
            new_cdsp = 2;
        } else if t_hvx >= 44000 {
            new_cdsp = 1;
        } else if t_hvx <= 42000 {
            new_cdsp = 0;
        }

        if new_cdsp != state_cdsp {
            state_cdsp = new_cdsp;
            println!("[CDSP] Shifted to Level {}", state_cdsp);
            write_opt!(node_cdsp, state_cdsp);
        }

        // ── Temp State (OEM thresholds) ─────────────────────────────────
        let mut new_tstate = state_tstate;
        for i in (0..4).rev() {
            if virtual_temp >= TSTATE_TRIG[i] && state_tstate <= i {
                new_tstate = i + 1;
                break;
            }
        }
        if new_tstate <= state_tstate {
            for i in 0..4 {
                if virtual_temp <= TSTATE_CLR[i] && state_tstate > i {
                    new_tstate = i;
                    break;
                }
            }
        }
        if new_tstate != state_tstate {
            state_tstate = new_tstate;
            if state_tstate == 0 {
                println!("[TEMP_STATE] Cleared");
                write_opt!(node_tstate, 0);
            } else {
                let val = TSTATE_TARGET[state_tstate - 1];
                println!("[TEMP_STATE] Set to {}", val);
                write_opt!(node_tstate, val);
            }
        }

        // ── Backlight (OEM thresholds) ────────────────────────────────────
        if virtual_temp >= BL_TRIG {
            let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);
            if current_bl > BL_LIMIT {
                println!("[BACKLIGHT] Critical Temp - Clamping brightness from {} to {}", current_bl, BL_LIMIT);
                write_opt!(node_backlight, BL_LIMIT);
                state_backlight_clamped = true;
            }
        } else if virtual_temp <= BL_CLR && state_backlight_clamped {
            println!("[BACKLIGHT] Cooled down - Releasing thermal clamp");
            state_backlight_clamped = false;
        }

        // ── WiFi (OEM thresholds) ──────────────────────────────────────────
        if virtual_temp >= WIFI_TRIG && state_wifi == 0 {
            state_wifi = 1;
            println!("[WIFI] Limited");
            write_opt!(node_wifi, 1);
        } else if virtual_temp <= WIFI_CLR && state_wifi == 1 {
            state_wifi = 0;
            println!("[WIFI] Limit removed");
            write_opt!(node_wifi, 0);
        }

        // ── WALT Input Boost (OEM thresholds) ──────────────────────────────
        let should_disable_boost = (virtual_temp >= BOOST_TRIG) || 
                                   (virtual_temp >= 35000 && is_thermal_spike);
        
        if should_disable_boost && state_boost == 1 {
            state_boost = 0;
            println!("[BOOST] Proactive Disable (Thermal cap reached)");
            write_str_opt!(node_walt_boost, BOOST_DISABLED_STR);
        } else if !should_disable_boost && virtual_temp <= BOOST_CLR && state_boost == 0 {
            state_boost = 1;
            println!("[BOOST] Re-enabled WALT Input Boost");
            write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);
        }

        // ── Hotplug ──────────────────────────────────────────────────────
        // MONITOR-CCC from OEM config
        if virtual_temp >= CCC_TRIG && !state_ccc_hotplug {
            state_ccc_hotplug = true;
            println!("[HOTPLUG-CCC] {}°C: disabling cores 2,3,6", virtual_temp / 1000);
        } else if virtual_temp <= CCC_CLR && state_ccc_hotplug {
            state_ccc_hotplug = false;
            println!("[HOTPLUG-CCC] Cooled: enabling cores 2,3,6");
        }

        // MONITOR-BCL from OEM config
        if soc <= 5 && !state_bcl_hotplug {
            state_bcl_hotplug = true;
            println!("[HOTPLUG-BCL] Battery <=5%: disabling cores 2,3,6");
        } else if soc >= 6 && state_bcl_hotplug {
            state_bcl_hotplug = false;
            println!("[HOTPLUG-BCL] Battery >5%: enabling cores 2,3,6");
        }

        let offline = state_ccc_hotplug || state_bcl_hotplug;
        let core_val = if offline { 0 } else { 1 };
        write_opt!(node_cpu2_on, core_val);
        write_opt!(node_cpu3_on, core_val);
        write_opt!(node_cpu6_on, core_val);

        // ── Smart Idle Charge Control ─────────────────────────────────────
        if soc >= 100 && screen_state == 0 && !charge_paused_at_full {
            if let Some(start_time) = full_charge_start_time {
                if start_time.elapsed().as_secs() >= 600 {
                    println!("[SMART CHARGE] 10 minutes elapsed at 100%. Suspending input current.");
                    write_opt!(node_input_suspend, 1);
                    charge_paused_at_full = true;
                    full_charge_start_time = None;
                }
            } else {
                println!("[SMART CHARGE] Battery 100% & Screen Off. Waiting 10 minutes to suspend.");
                full_charge_start_time = Some(Instant::now());
            }
        } else {
            if full_charge_start_time.is_some() {
                println!("[SMART CHARGE] Charge timer interrupted. Resetting.");
                full_charge_start_time = None;
            }

            if charge_paused_at_full && screen_state == 1 {
                println!("[SMART CHARGE] Screen turned on. Restoring charge input.");
                write_opt!(node_input_suspend, 0);
                charge_paused_at_full = false;
            }
        }

        // ── Charging Control Protocol Switching ─────────────────────────────
        // Check if USB is actually connected before attempting to charge
        if usb_online == 0 {
            // No charger connected - don't try to charge
            write_opt!(node_res_chg, 0);
            write_opt!(node_res_cur, 0);
            write_opt!(node_chg_limit, 0);
            write_opt!(node_input_suspend, 1);
        } else if !charge_paused_at_full {
            let is_xiaomi_charger = fastcharge_mode == 1 && quick_charge_type == 3;
            let is_qc2_charger = fastcharge_mode == 0 && quick_charge_type == 2;
            let is_std_5v_charger = fastcharge_mode == 0 && quick_charge_type == 0;
            let is_qc1_charger = fastcharge_mode == 0 && quick_charge_type == 1;

            if is_xiaomi_charger {
                if screen_state == 1 {
                    write_opt!(node_chg_limit, 0);
                    write_opt!(node_res_chg, 1);

                    let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);

                    // Screen-ON thresholds increased by +1.5°C (+1500)
                    let mut restrict_cur = if t_battery >= 43300 || virtual_temp >= 49000 {
                        200000 
                    } else if t_battery >= 41500 || virtual_temp >= 46500 {
                        500000 
                    } else if t_battery >= 40000 || virtual_temp >= 44500 {
                        1000000 
                    } else if is_thermal_spike {
                        800000 
                    } else {
                        1500000 
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 500000 {
                        restrict_cur -= 300000;
                    }

                    write_opt!(node_res_cur, restrict_cur);
                    restricted = false;
                } else {
                    if is_thermal_spike && batt_temp >= PROACTIVE_CHG_TEMP_THRESHOLD {
                        if !restricted {
                            println!("[CHG-XIAOMI] Proactive restrict mode due to rapid thermal delta");
                            restricted = true;
                        }
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 1500000);
                    } else if t_battery >= 41800 || virtual_temp >= 47500 {
                        restricted = true;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 200000);
                    } else if t_battery >= 41000 || virtual_temp >= 46000 {
                        restricted = true;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 1000000);
                    } else if t_battery >= 40000 || virtual_temp >= 44000 {
                        restricted = true;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 2000000);
                    } else {
                        if restricted {
                            println!("[CHG-XIAOMI] Battery cooled to {}°C - exiting direct restriction", batt_temp);
                            restricted = false;
                        }

                        write_opt!(node_res_chg, 0);
                        write_opt!(node_res_cur, 2000000);

                        let limit = if t_battery >= 38500 || virtual_temp >= 42000 {
                            4
                        } else if t_battery >= 37000 || virtual_temp >= 40000 {
                            1
                        } else {
                            0
                        };

                        write_opt!(node_chg_limit, limit);
                    }
                }
            } else if is_qc2_charger {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                if screen_state == 1 {
                    let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);

                    // Screen-ON thresholds increased by +1.5°C (+1500)
                    let mut restrict_cur = if t_battery >= 43300 || virtual_temp >= 49000 {
                        200000
                    } else if t_battery >= 41500 || virtual_temp >= 46000 {
                        400000
                    } else if t_battery >= 40000 || virtual_temp >= 44000 {
                        700000
                    } else if is_thermal_spike {
                        500000
                    } else {
                        1000000 
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 400000 {
                        restrict_cur -= 200000;
                    }
                    write_opt!(node_res_cur, restrict_cur);
                } else {
                    let restrict_cur = if t_battery >= 41800 || virtual_temp >= 47500 {
                        200000
                    } else if t_battery >= 41000 || virtual_temp >= 46000 {
                        800000
                    } else if t_battery >= 40000 || virtual_temp >= 44500 {
                        1400000
                    } else if t_battery >= 38500 || virtual_temp >= 42500 {
                        2000000
                    } else if t_battery >= 37000 || virtual_temp >= 40500 {
                        2600000
                    } else if is_thermal_spike {
                        1200000
                    } else {
                        3000000
                    };
                    write_opt!(node_res_cur, restrict_cur);
                }
            } else if is_std_5v_charger {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                if screen_state == 1 {
                    let current_bl = node_backlight.as_mut().and_then(|n| n.read()).unwrap_or(0);

                    // Screen-ON thresholds increased by +1.5°C (+1500)
                    let mut restrict_cur = if t_battery >= 43300 || virtual_temp >= 49000 {
                        200000 
                    } else if t_battery >= 41500 || virtual_temp >= 46000 {
                        500000
                    } else if t_battery >= 40000 || virtual_temp >= 44000 {
                        800000
                    } else if is_thermal_spike {
                        600000
                    } else {
                        1200000
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 500000 {
                        restrict_cur -= 200000;
                    }
                    write_opt!(node_res_cur, restrict_cur);
                } else {
                    let restrict_cur = if t_battery >= 41800 || virtual_temp >= 47500 {
                        200000
                    } else if t_battery >= 41000 || virtual_temp >= 46000 {
                        800000
                    } else if t_battery >= 40000 || virtual_temp >= 44500 {
                        1500000
                    } else if t_battery >= 38500 || virtual_temp >= 42500 {
                        2200000
                    } else if t_battery >= 37000 || virtual_temp >= 40500 {
                        2600000
                    } else if is_thermal_spike {
                        1200000
                    } else {
                        3000000
                    };
                    write_opt!(node_res_cur, restrict_cur);
                }
            } else if is_qc1_charger {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                // Screen-ON thresholds increased by +1.5°C (+1500)
                let restrict_cur = if t_battery >= 43300 || virtual_temp >= 49000 {
                    200000
                } else if t_battery >= 41500 || virtual_temp >= 46000 {
                    500000
                } else if screen_state == 1 {
                    1000000 
                } else {
                    2500000 
                };
                write_opt!(node_res_cur, restrict_cur);
            } else {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                // Screen-ON thresholds increased by +1.5°C (+1500)
                let fallback_cur = if t_battery >= 43300 {
                    200000
                } else if t_battery >= 41500 {
                    600000
                } else if screen_state == 1 {
                    1000000 
                } else {
                    2000000 
                };
                write_opt!(node_res_cur, fallback_cur);
            }
        }
        prev_screen_state = screen_state;

        // ── Thermal State Logging (once per minute) ──────────────────────
        if now.duration_since(last_log).as_secs() >= 60 {
            // Read current charge limit for logging
            let current_limit = node_res_cur.as_mut().and_then(|n| n.read()).unwrap_or(0);
            log_thermal_state(
                virtual_temp, t_battery, soc, 
                current_limit,
                usb_online, screen_state, state_cpu0, state_cpu4
            );
            last_log = now;
        }

        // ── Status Log ────────────────────────────────────────────────────
        println!(
            "V:{}°C | B:{}°C | HVX:{}°C | Δ:{:+.1}°C/s | C0:L{} C4:L{} G:L{} CDSP:L{} ADSP:L{} DAC:{} TS:L{} BL:{} W:{} Bs:{} HP:{}{} SOC:{}% | CHG:[FM:{} QC:{}] USB:{} S:{}",
            virtual_c, batt_temp, t_hvx / 1000, temp_delta_normalized as f32 / 1000.0,
            state_cpu0, state_cpu4, state_gpu, state_cdsp, state_adsp,
            if dac_connected { "Y" } else { "N" }, state_tstate,
            if state_backlight_clamped { "CLAMP" } else { "NORM" },
            state_wifi, state_boost,
            if state_ccc_hotplug { "C" } else { "c" },
            if state_bcl_hotplug { "B" } else { "b" },
            soc, fastcharge_mode, quick_charge_type,
            if usb_online == 1 { "Y" } else { "N" },
            if charge_paused_at_full { "Y" } else { "N" }
        );

        // ── Dynamic Sleep Duration ──────────────────────────────────────
        let sleep_duration = match screen_state {
            0 => Duration::from_secs(SCREEN_OFF_SLEEP),
            _ if virtual_temp >= 40000 || is_thermal_spike => Duration::from_secs(HOT_SLEEP),
            _ => Duration::from_secs(NORMAL_SLEEP),
        };

        thread::sleep(sleep_duration);
    }

    // ── Graceful Shutdown Hardware Restoration ────────────────────────────
    println!("\n[DAEMON] Terminating! Restoring hardware defaults...");
    write_opt!(node_cpu0, CPU0_DEFAULT);
    write_opt!(node_cpu4, CPU4_DEFAULT);
    write_opt!(node_gpu, GPU_FREQS[0]);
    write_opt!(node_tstate, 0);
    write_opt!(node_wifi, 0);
    write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);
    write_opt!(node_adsp, 0);
    write_opt!(node_cdsp, 0);
    write_opt!(node_res_chg, 0);
    write_opt!(node_chg_limit, 0);
    write_opt!(node_input_suspend, 0);
    write_opt!(node_cpu2_on, 1);
    write_opt!(node_cpu3_on, 1);
    write_opt!(node_cpu6_on, 1);
    
    std::process::exit(0);
}
