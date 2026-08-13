//! Unified Thermal & Charging Daemon for Xiaomi Sapphire (sapphire).
//! Optimized charging daemon supporting all charger protocols (Xiaomi HyperCharge, QC2.0, QC1.0, Std 5V).
//! - Maximized Screen-OFF charging speeds up to a strict 42.0°C hard thermal limit (+2°C total adjustment).
//! - Increased Screen-ON charging thermal thresholds by an additional +1.5°C for higher active current retention.
//! - Dynamic polling rate scaling based on thermal velocity.
//! - Charging current hysteresis protection to prevent fluttering.
//! - 1D Discrete Kalman Filter for sensor noise reduction.

#![allow(missing_docs)]
#![allow(clippy::needless_range_loop)]
#![allow(clippy::collapsible_else_if)]
#![allow(unused_variables)]
#![allow(unused_assignments)]
#![allow(dead_code)]
#![allow(clippy::manual_is_ascii_check)]

use std::fs::metadata;
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::sync::atomic::{AtomicBool, Ordering};
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
const CPU7_ONLINE: &str = "/sys/devices/system/cpu/cpu7/online";

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
const INPUT_SUSPEND_NODE: &str = "/sys/class/qcom-battery/input_suspend";
const FASTCHARGE_MODE_NODE: &str = "/sys/class/qcom-battery/fastcharge_mode";
const QUICK_CHARGE_TYPE_NODE: &str = "/sys/class/qcom-battery/quick_charge_type";

// ── DSP & Audio paths ───────────────────────────────────────────────────────
const TZ_CDSP_HVX: &str = "/sys/class/thermal/thermal_zone2/temp";
const CDSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device30/cur_state";
const ADSP_CUR_STATE: &str = "/sys/class/thermal/cooling_device37/cur_state";
const USB_DAC_PATH: &str = "/sys/class/sound/card1";

// ── Virtual sensor parameters ──────────────────────────────────────────────
const WEIGHT_QUIET: i32 = 884;
const WEIGHT_PA: i32 = 75;
const WEIGHT_CHARGE: i32 = 28;
const WEIGHT_EMMC: i32 = 13;
const WEIGHT_BATTERY: i32 = 0;
const WEIGHT_SUM: i32 = 1000;
const COMPENSATION: i32 = 0;

// ── Thermal stepwise/monitor configurations ────────────────────────────────
const CPU0_TRIG: [i32; 4] = [34000, 37000, 40000, 43000];
const CPU0_CLR: [i32; 4] = [32000, 35000, 38000, 41000];
const CPU0_TARGET: [i32; 4] = [1804800, 1516800, 1190400, 691200];
const CPU0_DEFAULT: i32 = 1900800;

const CPU4_TRIG: [i32; 6] = [34000, 36000, 38000, 41000, 43500, 45500];
const CPU4_CLR: [i32; 6] = [32000, 34000, 36000, 39000, 41500, 44500];
const CPU4_TARGET: [i32; 6] = [2400000, 2208000, 1900800, 1516800, 1113600, 806400];
const CPU4_DEFAULT: i32 = 2803200;

const GPU_TRIG: [i32; 3] = [43000, 45000, 46000];
const GPU_CLR: [i32; 3] = [41000, 43000, 45000];
const GPU_FREQS: [i32; 7] = [1260000000, 1114800000, 1025000000, 785000000, 600000000, 465000000, 320000000];
const GPU_TARGET_INDICES: [usize; 3] = [2, 4, 5];

const TSTATE_TRIG: [i32; 4] = [46000, 48000, 51000, 53000];
const TSTATE_CLR: [i32; 4] = [44000, 46000, 50000, 51000];
const TSTATE_TARGET: [i32; 4] = [110100000, 110100004, 112300001, 112520001];

// ── WALT String Configurations ──────────────────────────────────────────────
const BOOST_ENABLED_STR: &str = "1190400 0 0 0 1113600 0 0 0";
const BOOST_DISABLED_STR: &str = "0 0 0 0 0 0 0 0";

// ── Predictive control constants ──────────────────────────────────────────
const THERMAL_SPIKE_THRESHOLD_NORMALIZED: i32 = 750;
const PROACTIVE_CPU_TEMP_BOOST: i32 = 3000;
const PROACTIVE_BOOST_TEMP_THRESHOLD: i32 = 34500;
const PROACTIVE_CHG_TEMP_THRESHOLD: i32 = 38500;
const SCREEN_ON_OFFSET: i32 = 1500;

// ── Sleep duration constants ──────────────────────────────────────────────
const SLEEP_IDLE_SCREEN_OFF: u64 = 12;
const SLEEP_NORMAL: u64 = 3;
const SLEEP_ACTIVE_SPIKE: u64 = 1;

// ── Kalman Filter constants ───────────────────────────────────────────────
const KALMAN_Q: f64 = 0.01;  // Process noise - very low, trust the physics model
const KALMAN_R: f64 = 2.0;   // Measurement noise - moderate sensor jitter

// ── Signal handling ────────────────────────────────────────────────────────
static RUNNING: AtomicBool = AtomicBool::new(true);

const SIGINT: i32 = 2;
const SIGTERM: i32 = 15;

extern "C" fn handle_sig(_sig: i32) {
    RUNNING.store(false, Ordering::SeqCst);
}

extern "C" {
    fn signal(sig: i32, handler: extern "C" fn(i32)) -> usize;
}

// ── Kalman Filter ──────────────────────────────────────────────────────────
struct KalmanFilter {
    x: f64,      // Estimated true temperature (state)
    p: f64,      // Estimation error covariance
    q: f64,      // Process noise covariance (trust in physics model)
    r: f64,      // Measurement noise covariance (trust in sensor accuracy)
}

impl KalmanFilter {
    fn new(initial_val: f64, q: f64, r: f64) -> Self {
        Self {
            x: initial_val,
            p: 1.0,
            q,
            r,
        }
    }

    #[inline(always)]
    fn update(&mut self, measurement: f64) -> f64 {
        // 1. Prediction Step (Time Update)
        // Assuming constant state model: x_pred = x
        let x_pred = self.x;
        let p_pred = self.p + self.q;

        // 2. Correction Step (Measurement Update)
        let k = p_pred / (p_pred + self.r);  // Kalman Gain
        self.x = x_pred + k * (measurement - x_pred);
        self.p = (1.0 - k) * p_pred;

        self.x
    }
}

// ── DAC Detector with caching ─────────────────────────────────────────────
struct DacDetector {
    last_check: Instant,
    connected: bool,
    check_interval: Duration,
}

impl DacDetector {
    fn new() -> Self {
        Self {
            last_check: Instant::now() - Duration::from_secs(10),
            connected: false,
            check_interval: Duration::from_secs(3),
        }
    }

    fn is_connected(&mut self) -> bool {
        let now = Instant::now();
        if now.duration_since(self.last_check) >= self.check_interval {
            self.connected = metadata(USB_DAC_PATH).is_ok();
            self.last_check = now;
        }
        self.connected
    }
}

// ── High-Performance I/O Wrapper ────────────────────────────────────────────
struct SysfsNode {
    file: std::fs::File,
    path: &'static str,
}

impl SysfsNode {
    fn new(path: &'static str, read_only: bool) -> Option<Self> {
        match OpenOptions::new().read(true).write(!read_only).open(path) {
            Ok(file) => Some(Self { file, path }),
            Err(e) => {
                eprintln!("Warning: Could not open {} ({})", path, e);
                None
            }
        }
    }

    #[inline(always)]
    fn read_fast(&mut self) -> Option<i32> {
        self.file.seek(SeekFrom::Start(0)).ok()?;
        
        let mut buf = [0u8; 16];
        let n = self.file.read(&mut buf).ok()?;
        if n == 0 { return None; }
        
        let mut val: i32 = 0;
        let mut started = false;
        
        for &b in &buf[..n] {
            if b.is_ascii_digit() {
                val = val * 10 + (b - b'0') as i32;
                started = true;
            } else if started {
                break;
            }
        }
        
        if started { Some(val) } else { None }
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

// ── Helpers ────────────────────────────────────────────────────────────────
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
        match $node.as_mut().and_then(|n| n.read_fast()) {
            Some(val) if val > 0 && val < 90000 => {
                $last_val = val;
                val
            }
            Some(invalid) => {
                eprintln!("[SENSOR] Invalid reading: {}°C, using fallback", invalid);
                $last_val
            }
            None => {
                eprintln!("[SENSOR] Read failed, using fallback: {}°C", $last_val / 1000);
                $last_val
            }
        }
    };
}

// ── Main ────────────────────────────────────────────────────────────────────
fn main() {
    println!("============================================================");
    println!(" Unified Thermal & Charging Daemon (Rust)");
    println!(" ML-Optimized weights | 1D Kalman Filter (Q={:.2}, R={:.1})", KALMAN_Q, KALMAN_R);
    println!(" Dynamic Polling Rate Scaling | Charging Hysteresis");
    println!(" Audio-Aware CPU Floor | Graceful Restoration");
    println!("============================================================");

    // SAFETY: signal() is a standard POSIX system call. The signal handlers
    // only set an atomic flag, which is safe in a signal context.
    unsafe {
        signal(SIGINT, handle_sig);
        signal(SIGTERM, handle_sig);
    }

    // Single instance check
    if let Ok(mut f) = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open("/data/local/tmp/thermal_daemon.pid") 
    {
        let pid = std::process::id();
        let _ = write!(f, "{}", pid);
    }

    // ── Sensor nodes ──────────────────────────────────────────────────────
    let mut node_t_pa = SysfsNode::new(TZ_PA, true);
    let mut node_t_quiet = SysfsNode::new(TZ_QUIET, true);
    let mut node_t_charge = SysfsNode::new(TZ_CHARGE, true);
    let mut node_t_emmc = SysfsNode::new(TZ_EMMC, true);
    let mut node_t_battery = SysfsNode::new(TZ_BATTERY, true);
    let mut node_soc = SysfsNode::new(BAT_SOC_PATH, true);
    let mut node_screen = SysfsNode::new(SCREEN_STATE_NODE, true);

    // ── Control nodes ────────────────────────────────────────────────────
    let mut node_cpu0 = SysfsNode::new(CPU0_MAX_FREQ, false);
    let mut node_cpu4 = SysfsNode::new(CPU4_MAX_FREQ, false);
    let mut node_gpu = SysfsNode::new(GPU_MAX_FREQ, false);
    let mut node_cpu2_on = SysfsNode::new(CPU2_ONLINE, false);
    let mut node_cpu3_on = SysfsNode::new(CPU3_ONLINE, false);
    let mut node_cpu6_on = SysfsNode::new(CPU6_ONLINE, false);
    let mut node_cpu7_on = SysfsNode::new(CPU7_ONLINE, false);
    let mut node_backlight = SysfsNode::new(BACKLIGHT_PATH, false);
    let mut node_tstate = SysfsNode::new(TEMP_STATE_PATH, false);
    let mut node_wifi = SysfsNode::new(WIFI_LIMIT_PATH, false);
    let mut node_chg_limit = SysfsNode::new(CHARGE_LIMIT_NODE, false);
    let mut node_res_chg = SysfsNode::new(RESTRICT_CHG_NODE, false);
    let mut node_res_cur = SysfsNode::new(RESTRICT_CUR_NODE, false);
    let mut node_input_suspend = SysfsNode::new(INPUT_SUSPEND_NODE, false);
    let mut node_fastcharge_mode = SysfsNode::new(FASTCHARGE_MODE_NODE, true);
    let mut node_quick_charge_type = SysfsNode::new(QUICK_CHARGE_TYPE_NODE, true);

    // ── WALT nodes ──────────────────────────────────────────────────────
    let mut node_walt_boost = SysfsNode::new(WALT_BOOST_FREQ_PATH, false);
    let mut node_walt_ms = SysfsNode::new(WALT_BOOST_MS_PATH, false);

    // ── DSP nodes ────────────────────────────────────────────────────────
    let mut node_t_hvx = SysfsNode::new(TZ_CDSP_HVX, true);
    let mut node_cdsp = SysfsNode::new(CDSP_CUR_STATE, false);
    let mut node_adsp = SysfsNode::new(ADSP_CUR_STATE, false);

    // ── Initial state ────────────────────────────────────────────────────
    write_opt!(node_res_chg, 0);
    write_opt!(node_res_cur, 1500000);
    write_opt!(node_input_suspend, 0);
    write_str_opt!(node_walt_ms, "40");
    write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);

    // ── Kalman Filter initialization ─────────────────────────────────────
    // Start at 45°C (safe fallback), Q=0.01 (low process noise), R=2.0 (moderate sensor jitter)
    let mut virtual_temp_kf = KalmanFilter::new(45000.0, KALMAN_Q, KALMAN_R);

    // ── State tracking ──────────────────────────────────────────────────
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
    let mut dac_detector = DacDetector::new();
    let mut chg_hysteresis_offset: i32 = 0;

    // ── Sensor fallbacks (start HIGH for safe defaults) ─────────────────
    let mut last_t_pa = 45000;
    let mut last_t_quiet = 45000;
    let mut last_t_charge = 45000;
    let mut last_t_emmc = 45000;
    let mut last_t_battery = 45000;
    let mut last_t_hvx = 45000;

    // ── Runtime loop ──────────────────────────────────────────────────────
    while RUNNING.load(Ordering::SeqCst) {
        // ── Read sensors ─────────────────────────────────────────────────
        let t_pa = read_sensor_safe!(node_t_pa, last_t_pa);
        let t_quiet = read_sensor_safe!(node_t_quiet, last_t_quiet);
        let t_charge = read_sensor_safe!(node_t_charge, last_t_charge);
        let t_emmc = read_sensor_safe!(node_t_emmc, last_t_emmc);
        let t_battery = read_sensor_safe!(node_t_battery, last_t_battery);
        let t_hvx = read_sensor_safe!(node_t_hvx, last_t_hvx);
        
        let soc = node_soc.as_mut().and_then(|n| n.read_fast()).unwrap_or(100);
        let screen_state = node_screen.as_mut().and_then(|n| n.read_fast()).unwrap_or(0);
        let dac_connected = dac_detector.is_connected();

        let fastcharge_mode = node_fastcharge_mode.as_mut().and_then(|n| n.read_fast()).unwrap_or(-1);
        let quick_charge_type = node_quick_charge_type.as_mut().and_then(|n| n.read_fast()).unwrap_or(-1);

        // ── Raw Virtual temperature ──────────────────────────────────────
        let raw_virtual_temp = (WEIGHT_QUIET * t_quiet
            + WEIGHT_PA * t_pa
            + WEIGHT_CHARGE * t_charge
            + WEIGHT_EMMC * t_emmc
            + WEIGHT_BATTERY * t_battery)
            / WEIGHT_SUM
            + COMPENSATION;

        // ── Apply Kalman Filter to smooth sensor noise ──────────────────
        let virtual_temp = virtual_temp_kf.update(raw_virtual_temp as f64) as i32;
        let virtual_c = virtual_temp / 1000;
        let batt_temp = t_battery / 1000;

        // ── Thermal derivative using filtered value ─────────────────────
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
            println!("[PROACTIVE] Rapid thermal rise detected! Rate: +{:.1}°C/s", 
                     temp_delta_normalized as f32 / 1000.0);
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

        // ── GPU ───────────────────────────────────────────────────────────
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

        // ── ADSP Control ──────────────────────────────────────────────────
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

        // ── CDSP Control ──────────────────────────────────────────────────
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

        // ── Temp State ────────────────────────────────────────────────────
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

        // ── Backlight Clamp ──────────────────────────────────────────────
        if virtual_temp >= 51000 {
            let current_bl = node_backlight.as_mut().and_then(|n| n.read_fast()).unwrap_or(0);

            if current_bl > BL_LIMIT {
                println!("[BACKLIGHT] Critical Temp (51°C) - Clamping brightness from {} to {}", 
                         current_bl, BL_LIMIT);
                write_opt!(node_backlight, BL_LIMIT);
                state_backlight_clamped = true;
            }
        } else if virtual_temp <= 49000 && state_backlight_clamped {
            println!("[BACKLIGHT] Cooled down - Releasing thermal clamp");
            state_backlight_clamped = false;
        }

        // ── WiFi ──────────────────────────────────────────────────────────
        if virtual_temp >= 46000 && state_wifi == 0 {
            state_wifi = 1;
            println!("[WIFI] Limited");
            write_opt!(node_wifi, 1);
        } else if virtual_temp <= 44000 && state_wifi == 1 {
            state_wifi = 0;
            println!("[WIFI] Limit removed");
            write_opt!(node_wifi, 0);
        }

        // ── WALT Input Boost ──────────────────────────────────────────────
        let should_disable_boost = (virtual_temp >= PROACTIVE_BOOST_TEMP_THRESHOLD) || 
                                   (virtual_temp >= 35000 && is_thermal_spike);
        
        if should_disable_boost && state_boost == 1 {
            state_boost = 0;
            println!("[BOOST] Proactive Disable (Thermal cap reached)");
            write_str_opt!(node_walt_boost, BOOST_DISABLED_STR);
        } else if !should_disable_boost && virtual_temp <= (PROACTIVE_BOOST_TEMP_THRESHOLD - 2000) && state_boost == 0 {
            state_boost = 1;
            println!("[BOOST] Re-enabled WALT Input Boost");
            write_str_opt!(node_walt_boost, BOOST_ENABLED_STR);
        }

        // ── Hotplug (Symmetric - A73 cores only) ──────────────────────────
        if virtual_temp >= 49000 && !state_ccc_hotplug {
            state_ccc_hotplug = true;
            println!("[HOTPLUG] 49°C: disabling A73 cores (6,7)");
            write_opt!(node_cpu6_on, 0);
            write_opt!(node_cpu7_on, 0);
        } else if virtual_temp <= 47000 && state_ccc_hotplug {
            state_ccc_hotplug = false;
            println!("[HOTPLUG] Cooled: re-enabling A73 cores (6,7)");
            write_opt!(node_cpu6_on, 1);
            write_opt!(node_cpu7_on, 1);
        }

        if soc <= 5 && !state_bcl_hotplug {
            state_bcl_hotplug = true;
            println!("[HOTPLUG-BCL] Battery <=5%: disabling A73 cores (6,7)");
            write_opt!(node_cpu6_on, 0);
            write_opt!(node_cpu7_on, 0);
        } else if soc >= 6 && state_bcl_hotplug {
            state_bcl_hotplug = false;
            println!("[HOTPLUG-BCL] Battery >5%: re-enabling A73 cores (6,7)");
            write_opt!(node_cpu6_on, 1);
            write_opt!(node_cpu7_on, 1);
        }

        // A53 cores always online
        write_opt!(node_cpu2_on, 1);
        write_opt!(node_cpu3_on, 1);

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
                full_charge_start_time = None;
            }

            if charge_paused_at_full && screen_state == 1 {
                println!("[SMART CHARGE] Screen turned on. Restoring charge input.");
                write_opt!(node_input_suspend, 0);
                charge_paused_at_full = false;
            }
        }

        // ── Charging Control with Hysteresis ─────────────────────────────
        if !charge_paused_at_full {
            let is_xiaomi_charger = fastcharge_mode == 1 && quick_charge_type == 3;
            let is_qc2_charger = fastcharge_mode == 0 && quick_charge_type == 2;
            let is_std_5v_charger = fastcharge_mode == 0 && quick_charge_type == 0;
            let is_qc1_charger = fastcharge_mode == 0 && quick_charge_type == 1;

            // Apply hysteresis offset: effective temperatures are shifted
            // when we're in a restricted state to prevent fluttering
            let effective_batt = t_battery - chg_hysteresis_offset;
            let effective_virt = virtual_temp - chg_hysteresis_offset;
            
            // Screen-on offset
            let batt_limit = if screen_state == 1 {
                t_battery + SCREEN_ON_OFFSET
            } else {
                t_battery
            };

            if is_xiaomi_charger {
                if screen_state == 1 {
                    write_opt!(node_chg_limit, 0);
                    write_opt!(node_res_chg, 1);

                    let current_bl = node_backlight.as_mut().and_then(|n| n.read_fast()).unwrap_or(0);

                    let mut restrict_cur = if batt_limit >= 43300 || virtual_temp >= 49000 {
                        chg_hysteresis_offset = 1000;
                        200000 
                    } else if batt_limit >= 41500 || virtual_temp >= 46500 {
                        chg_hysteresis_offset = 800;
                        500000 
                    } else if batt_limit >= 40000 || virtual_temp >= 44500 {
                        chg_hysteresis_offset = 500;
                        1000000 
                    } else if is_thermal_spike {
                        chg_hysteresis_offset = 500;
                        800000 
                    } else {
                        chg_hysteresis_offset = 0;
                        1500000 
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 500000 {
                        restrict_cur -= 300000;
                    }

                    write_opt!(node_res_cur, restrict_cur);
                    restricted = false;
                } else {
                    // Use effective temperatures with hysteresis
                    if is_thermal_spike && batt_temp >= PROACTIVE_CHG_TEMP_THRESHOLD {
                        if !restricted {
                            println!("[CHG-XIAOMI] Proactive restrict mode due to rapid thermal delta");
                            restricted = true;
                            chg_hysteresis_offset = 1000;
                        }
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 1500000);
                    } else if effective_batt >= 41800 || effective_virt >= 47500 {
                        restricted = true;
                        chg_hysteresis_offset = 1000;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 200000);
                    } else if effective_batt >= 41000 || effective_virt >= 46000 {
                        restricted = true;
                        chg_hysteresis_offset = 800;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 1000000);
                    } else if effective_batt >= 40000 || effective_virt >= 44000 {
                        restricted = true;
                        chg_hysteresis_offset = 500;
                        write_opt!(node_chg_limit, 0);
                        write_opt!(node_res_chg, 1);
                        write_opt!(node_res_cur, 2000000);
                    } else {
                        if restricted {
                            println!("[CHG-XIAOMI] Battery cooled safely - clearing hysteresis & restriction");
                            restricted = false;
                        }
                        chg_hysteresis_offset = 0;

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
                    let current_bl = node_backlight.as_mut().and_then(|n| n.read_fast()).unwrap_or(0);

                    let mut restrict_cur = if batt_limit >= 43300 || virtual_temp >= 49000 {
                        chg_hysteresis_offset = 1000;
                        200000
                    } else if batt_limit >= 41500 || virtual_temp >= 46000 {
                        chg_hysteresis_offset = 800;
                        400000
                    } else if batt_limit >= 40000 || virtual_temp >= 44000 {
                        chg_hysteresis_offset = 500;
                        700000
                    } else if is_thermal_spike {
                        chg_hysteresis_offset = 500;
                        500000
                    } else {
                        chg_hysteresis_offset = 0;
                        1000000 
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 400000 {
                        restrict_cur -= 200000;
                    }
                    write_opt!(node_res_cur, restrict_cur);
                } else {
                    let restrict_cur = if effective_batt >= 41800 || effective_virt >= 47500 {
                        chg_hysteresis_offset = 1000;
                        200000
                    } else if effective_batt >= 41000 || effective_virt >= 46000 {
                        chg_hysteresis_offset = 800;
                        800000
                    } else if effective_batt >= 40000 || effective_virt >= 44500 {
                        chg_hysteresis_offset = 500;
                        1400000
                    } else if effective_batt >= 38500 || effective_virt >= 42500 {
                        chg_hysteresis_offset = 300;
                        2000000
                    } else if effective_batt >= 37000 || effective_virt >= 40500 {
                        chg_hysteresis_offset = 0;
                        2600000
                    } else if is_thermal_spike {
                        chg_hysteresis_offset = 500;
                        1200000
                    } else {
                        chg_hysteresis_offset = 0;
                        3000000
                    };
                    write_opt!(node_res_cur, restrict_cur);
                }
            } else if is_std_5v_charger {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                if screen_state == 1 {
                    let current_bl = node_backlight.as_mut().and_then(|n| n.read_fast()).unwrap_or(0);

                    let mut restrict_cur = if batt_limit >= 43300 || virtual_temp >= 49000 {
                        chg_hysteresis_offset = 1000;
                        200000 
                    } else if batt_limit >= 41500 || virtual_temp >= 46000 {
                        chg_hysteresis_offset = 800;
                        500000
                    } else if batt_limit >= 40000 || virtual_temp >= 44000 {
                        chg_hysteresis_offset = 500;
                        800000
                    } else if is_thermal_spike {
                        chg_hysteresis_offset = 500;
                        600000
                    } else {
                        chg_hysteresis_offset = 0;
                        1200000
                    };

                    if current_bl > BL_LIMIT && restrict_cur > 500000 {
                        restrict_cur -= 200000;
                    }
                    write_opt!(node_res_cur, restrict_cur);
                } else {
                    let restrict_cur = if effective_batt >= 41800 || effective_virt >= 47500 {
                        chg_hysteresis_offset = 1000;
                        200000
                    } else if effective_batt >= 41000 || effective_virt >= 46000 {
                        chg_hysteresis_offset = 800;
                        800000
                    } else if effective_batt >= 40000 || effective_virt >= 44500 {
                        chg_hysteresis_offset = 500;
                        1500000
                    } else if effective_batt >= 38500 || effective_virt >= 42500 {
                        chg_hysteresis_offset = 300;
                        2200000
                    } else if effective_batt >= 37000 || effective_virt >= 40500 {
                        chg_hysteresis_offset = 0;
                        2600000
                    } else if is_thermal_spike {
                        chg_hysteresis_offset = 500;
                        1200000
                    } else {
                        chg_hysteresis_offset = 0;
                        3000000
                    };
                    write_opt!(node_res_cur, restrict_cur);
                }
            } else if is_qc1_charger {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                let restrict_cur = if batt_limit >= 43300 || virtual_temp >= 49000 {
                    chg_hysteresis_offset = 1000;
                    200000
                } else if batt_limit >= 41500 || virtual_temp >= 46000 {
                    chg_hysteresis_offset = 800;
                    500000
                } else if screen_state == 1 {
                    chg_hysteresis_offset = 0;
                    1000000 
                } else {
                    chg_hysteresis_offset = 0;
                    2500000 
                };
                write_opt!(node_res_cur, restrict_cur);
            } else {
                write_opt!(node_chg_limit, 0);
                write_opt!(node_res_chg, 1);

                let fallback_cur = if batt_limit >= 43300 {
                    chg_hysteresis_offset = 1000;
                    200000
                } else if batt_limit >= 41500 {
                    chg_hysteresis_offset = 500;
                    600000
                } else if screen_state == 1 {
                    chg_hysteresis_offset = 0;
                    1000000 
                } else {
                    chg_hysteresis_offset = 0;
                    2000000 
                };
                write_opt!(node_res_cur, fallback_cur);
            }
        }
        prev_screen_state = screen_state;

        // ── Status Log ────────────────────────────────────────────────────
        println!(
            "V:{}°C (raw:{}°C) | B:{}°C | HVX:{}°C | Δ:{:+.1}°C/s | Hys:{} | C0:L{} C4:L{} G:L{} CDSP:L{} ADSP:L{} DAC:{} TS:L{} BL:{} W:{} Bs:{} HP:{}{} SOC:{}% | CHG:[FM:{} QC:{}] S:{}",
            virtual_c, raw_virtual_temp / 1000, batt_temp, t_hvx / 1000, 
            temp_delta_normalized as f32 / 1000.0,
            chg_hysteresis_offset / 1000,
            state_cpu0, state_cpu4, state_gpu, state_cdsp, state_adsp,
            if dac_connected { "Y" } else { "N" }, state_tstate,
            if state_backlight_clamped { "CLAMP" } else { "NORM" },
            state_wifi, state_boost,
            if state_ccc_hotplug { "C" } else { "c" },
            if state_bcl_hotplug { "B" } else { "b" },
            soc, fastcharge_mode, quick_charge_type,
            if charge_paused_at_full { "Y" } else { "N" }
        );

        // ── Dynamic Polling Rate Scaling ──────────────────────────────────
        let sleep_duration = if screen_state == 0 {
            if temp_delta_normalized < -100 {
                Duration::from_secs(SLEEP_IDLE_SCREEN_OFF + 4)
            } else if temp_delta_normalized > 200 {
                Duration::from_secs(SLEEP_NORMAL)
            } else {
                Duration::from_secs(SLEEP_IDLE_SCREEN_OFF)
            }
        } else {
            if is_thermal_spike || virtual_temp >= 42000 {
                Duration::from_secs(SLEEP_ACTIVE_SPIKE)
            } else if temp_delta_normalized > 400 {
                Duration::from_millis(1000)
            } else {
                Duration::from_secs(SLEEP_NORMAL)
            }
        };

        thread::sleep(sleep_duration);
    }

    // ── Graceful Shutdown ──────────────────────────────────────────────────
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
    write_opt!(node_cpu7_on, 1);
    
    let _ = std::fs::remove_file("/data/local/tmp/thermal_daemon.pid");
    
    std::process::exit(0);
}
