/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicU64, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crate::bindings::*;
use crate::display::DisplayController;
use crate::monitors::{DisplayMonitor, FodPressMonitor, ScreenStateMonitor};
use crate::touch::TouchController;

/// Main UDFPS handler for Xiaomi SM6225 devices
pub struct XiaomiSm6225UdfpsHandler {
    device: *mut fingerprint_device_t,
    touch_controller: Arc<TouchController>,
    display_controller: Arc<DisplayController>,
    
    // State
    enrolling: AtomicBool,
    is_running: AtomicBool,
    pending_cleanup: AtomicBool,
    hbm_stuck: AtomicBool,
    is_finger_down: AtomicBool,
    fb_down_time_ms: AtomicU64,
    
    // Monitors
    fod_monitor: Arc<FodPressMonitor>,
    display_monitor: Arc<DisplayMonitor>,
    screen_monitor: Option<Arc<ScreenStateMonitor>>,
    
    // Thread handles
    cleanup_thread: Mutex<Option<thread::JoinHandle<()>>>,
    
    // Flags
    is_fpc_fod: bool,
}

impl XiaomiSm6225UdfpsHandler {
    pub fn new() -> Self {
        let touch_ctrl = Arc::new(TouchController::new());
        let display_ctrl = Arc::new(DisplayController::new());
        
        let fod_monitor = Arc::new(FodPressMonitor::new(
            Arc::clone(&touch_ctrl),
            Arc::clone(&display_ctrl),
        ));
        
        let display_monitor = Arc::new(DisplayMonitor::new(
            Arc::clone(&display_ctrl),
        ));
        
        Self {
            device: ptr::null_mut(),
            touch_controller: touch_ctrl,
            display_controller: display_ctrl,
            enrolling: AtomicBool::new(false),
            is_running: AtomicBool::new(true),
            pending_cleanup: AtomicBool::new(false),
            hbm_stuck: AtomicBool::new(false),
            is_finger_down: AtomicBool::new(false),
            fb_down_time_ms: AtomicU64::new(0),
            fod_monitor,
            display_monitor,
            screen_monitor: None,
            cleanup_thread: Mutex::new(None),
            is_fpc_fod: false,
        }
    }

    /// Initialize the handler
    pub fn init(&mut self, device: *mut fingerprint_device_t) {
        log::info!("Initializing UDFPS handler (Rust)");
        self.device = device;

        // Initialize controllers
        self.touch_controller.init();
        self.display_controller.init();

        // Determine fingerprint vendor
        let fp_vendor = android_properties::get_str("persist.vendor.sys.fp.vendor", "none");
        log::info!("Fingerprint vendor: {}", fp_vendor);
        self.is_fpc_fod = fp_vendor == "fpc_fod";

        // Initialize monitors
        self.fod_monitor.set_device(device);
        self.display_monitor.set_device(device);
        self.fod_monitor.start();
        self.display_monitor.start();

        // Start screen monitor for FPC
        if self.is_fpc_fod {
            let screen_monitor = Arc::new(ScreenStateMonitor::new(
                Arc::clone(&self.touch_controller),
                Arc::clone(&self.display_controller),
            ));
            screen_monitor.set_device(device);
            screen_monitor.start();
            self.screen_monitor = Some(screen_monitor);
        }

        log::info!("UDFPS handler initialized (Rust)");
    }

    /// Schedule HBM cleanup
    fn schedule_hbm_cleanup(&self) {
        let mut guard = self.cleanup_thread.lock().unwrap();
        if let Some(thread) = guard.take() {
            let _ = thread.join();
        }

        let hbm_stuck = Arc::new(self.hbm_stuck.clone());
        let is_finger_down = Arc::new(self.is_finger_down.clone());
        let touch_ctrl = Arc::clone(&self.touch_controller);
        let display_ctrl = Arc::clone(&self.display_controller);

        *guard = Some(thread::spawn(move || {
            thread::sleep(Duration::from_millis(CLEANUP_DELAY_MS));
            if hbm_stuck.load(Ordering::SeqCst) && is_finger_down.load(Ordering::SeqCst) {
                log::info!("💡 Force cleaning HBM stuck state");
                touch_ctrl.reset_state();
                display_ctrl.force_hbm_off();
                hbm_stuck.store(false, Ordering::SeqCst);
            }
        }));
    }

    /// Force cleanup if finger is pressed
    fn force_cleanup_if_pressed(&self) {
        let pressed = self.fod_monitor.is_pressed();

        self.set_finger_down(false);

        if pressed {
            self.pending_cleanup.store(true, Ordering::SeqCst);
            log::info!("UDFPS: Finger held during enrollment finish. Cleanup deferred until lift.");

            let mut guard = self.cleanup_thread.lock().unwrap();
            if let Some(thread) = guard.take() {
                let _ = thread.join();
            }

            let pending_cleanup = Arc::new(self.pending_cleanup.clone());
            let hbm_stuck = Arc::new(self.hbm_stuck.clone());
            let enrolling = Arc::new(self.enrolling.clone());
            let touch_ctrl = Arc::clone(&self.touch_controller);
            let display_ctrl = Arc::clone(&self.display_controller);

            *guard = Some(thread::spawn(move || {
                thread::sleep(Duration::from_millis(FORCE_CLEANUP_DELAY_MS));
                if pending_cleanup.load(Ordering::SeqCst) {
                    log::info!("⚠️ Finger still held after 3 seconds, forcing cleanup");
                    touch_ctrl.reset_state();
                    display_ctrl.force_hbm_off();
                    pending_cleanup.store(false, Ordering::SeqCst);
                    hbm_stuck.store(false, Ordering::SeqCst);
                    if !enrolling.load(Ordering::SeqCst) {
                        touch_ctrl.set_fod_status(FOD_STATUS_OFF);
                    }
                }
            }));
        } else {
            self.pending_cleanup.store(false, Ordering::SeqCst);
            self.hbm_stuck.store(false, Ordering::SeqCst);
            self.touch_controller.set_fod_status(FOD_STATUS_OFF);
        }
    }

    /// Set finger down state
    fn set_finger_down(&self, pressed: bool) {
        self.touch_controller.set_finger_down(pressed);
        self.display_controller.set_hbm(pressed);
        
        // Send ext cmd to device
        unsafe {
            if !self.device.is_null() {
                if let Some(ext_cmd) = (*self.device).ext_cmd {
                    let cmd = COMMAND_FOD_PRESS_STATUS;
                    let param = if pressed { PARAM_FOD_PRESSED } else { PARAM_FOD_RELEASED };
                    ext_cmd(self.device, cmd, param);
                }
            }
        }

        self.is_finger_down.store(pressed, Ordering::SeqCst);
    }
}

/// UDFPS handler trait (matches C++ interface)
pub trait UdfpsHandler: Send + Sync {
    fn init(&mut self, device: *mut fingerprint_device_t);
    fn on_finger_down(&mut self, x: u32, y: u32, minor: f32, major: f32);
    fn on_finger_up(&mut self);
    fn on_acquired(&mut self, result: i32, vendor_code: i32);
    fn cancel(&mut self);
    fn pre_enroll(&mut self);
    fn enroll(&mut self);
    fn post_enroll(&mut self);
}

impl UdfpsHandler for XiaomiSm6225UdfpsHandler {
    fn init(&mut self, device: *mut fingerprint_device_t) {
        self.init(device);
    }

    fn on_finger_down(&mut self, _x: u32, _y: u32, _minor: f32, _major: f32) {
        log::info!("on_finger_down (Rust)");
        
        self.hbm_stuck.store(false, Ordering::SeqCst);
        
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;
        self.fb_down_time_ms.store(now, Ordering::SeqCst);

        if self.is_fpc_fod {
            self.touch_controller.set_fod_status(FOD_STATUS_ON);
        }

        self.set_finger_down(true);
    }

    fn on_finger_up(&mut self) {
        log::info!("on_finger_up (Rust)");
        self.set_finger_down(false);
        self.pending_cleanup.store(false, Ordering::SeqCst);
        self.hbm_stuck.store(false, Ordering::SeqCst);

        if !self.enrolling.load(Ordering::SeqCst) {
            self.touch_controller.set_fod_status(FOD_STATUS_OFF);
        }
    }

    fn on_acquired(&mut self, result: i32, vendor_code: i32) {
        log::info!("on_acquired result: {} vendorCode: {} (Rust)", result, vendor_code);

        if let Ok(acquired) = AcquiredInfo::try_from(result) {
            if acquired == AcquiredInfo::Good {
                self.set_finger_down(false);
                self.pending_cleanup.store(false, Ordering::SeqCst);
                self.hbm_stuck.store(false, Ordering::SeqCst);

                if !self.enrolling.load(Ordering::SeqCst) {
                    self.touch_controller.set_fod_status(FOD_STATUS_OFF);
                }
            }
        }

        if !self.is_fpc_fod && vendor_code == 21 {
            self.touch_controller.set_fod_status(FOD_STATUS_ON);
        } else if self.is_fpc_fod && vendor_code == 22 {
            self.touch_controller.set_fod_status(FOD_STATUS_ON);
        }

        // Detect HBM kill while finger is down
        if vendor_code == 23 && self.is_finger_down.load(Ordering::SeqCst) {
            log::info!("⚠️ HBM killed while finger is still down - potential stuck state detected");
            self.hbm_stuck.store(true, Ordering::SeqCst);
            self.schedule_hbm_cleanup();
        }
    }

    fn cancel(&mut self) {
        log::info!("cancel (Rust)");
        self.enrolling.store(false, Ordering::SeqCst);
        self.force_cleanup_if_pressed();
    }

    fn pre_enroll(&mut self) {
        log::info!("pre_enroll (Rust)");
        self.pending_cleanup.store(false, Ordering::SeqCst);
        self.hbm_stuck.store(false, Ordering::SeqCst);
        self.enrolling.store(true, Ordering::SeqCst);
    }

    fn enroll(&mut self) {
        log::info!("enroll (Rust)");
        self.enrolling.store(true, Ordering::SeqCst);
    }

    fn post_enroll(&mut self) {
        log::info!("post_enroll (Rust)");
        self.enrolling.store(false, Ordering::SeqCst);
        self.force_cleanup_if_pressed();
    }
}

impl Drop for XiaomiSm6225UdfpsHandler {
    fn drop(&mut self) {
        log::info!("Dropping UDFPS handler (Rust)");
        self.is_running.store(false, Ordering::SeqCst);
        
        // Stop monitors
        self.fod_monitor.stop();
        self.display_monitor.stop();
        if let Some(ref monitor) = self.screen_monitor {
            monitor.stop();
        }

        // Join cleanup thread
        if let Ok(mut guard) = self.cleanup_thread.lock() {
            if let Some(thread) = guard.take() {
                let _ = thread.join();
            }
        }

        // Clean up devices
        self.touch_controller.close();
        self.display_controller.close();
    }
}
