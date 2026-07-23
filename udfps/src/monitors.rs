/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

use std::fs::File;
use std::io::Read;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use nix::poll::{poll, PollFd, PollFlags};
use nix::unistd::read;

use crate::bindings::*;
use crate::display::DisplayController;
use crate::touch::TouchController;

/// Monitor for FOD press status
pub struct FodPressMonitor {
    touch_controller: Arc<TouchController>,
    display_controller: Arc<DisplayController>,
    device: *mut fingerprint_device_t,
    running: AtomicBool,
    thread: Option<thread::JoinHandle<()>>,
}

impl FodPressMonitor {
    pub fn new(touch: Arc<TouchController>, display: Arc<DisplayController>) -> Self {
        Self {
            touch_controller: touch,
            display_controller: display,
            device: std::ptr::null_mut(),
            running: AtomicBool::new(false),
            thread: None,
        }
    }

    pub fn set_device(&mut self, device: *mut fingerprint_device_t) {
        self.device = device;
    }

    pub fn start(&mut self) {
        if self.running.load(Ordering::SeqCst) {
            return;
        }

        self.running.store(true, Ordering::SeqCst);
        let touch = Arc::clone(&self.touch_controller);
        let display = Arc::clone(&self.display_controller);
        let running = Arc::new(self.running.clone());

        self.thread = Some(thread::spawn(move || {
            Self::monitor_thread(touch, display, running);
        }));
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::SeqCst);
        if let Some(thread) = self.thread.as_ref() {
            let _ = thread.join();
        }
    }

    pub fn is_pressed(&self) -> bool {
        self.touch_controller.read_fod_press_status()
    }

    fn monitor_thread(
        touch: Arc<TouchController>,
        display: Arc<DisplayController>,
        running: Arc<AtomicBool>,
    ) {
        log::info!("FOD press monitor thread started (Rust)");

        let path = std::path::Path::new(FOD_PRESS_STATUS_PATH);
        let mut file = match File::open(path) {
            Ok(f) => f,
            Err(e) => {
                log::error!("Failed to open {}: {}", FOD_PRESS_STATUS_PATH, e);
                return;
            }
        };

        let _ = touch.read_fod_press_status();

        let raw_fd = file.as_raw_fd();
        let mut poll_fd = PollFd::new(raw_fd, PollFlags::POLLERR | PollFlags::POLLPRI);

        while running.load(Ordering::SeqCst) {
            match poll(&mut [&mut poll_fd], 1000) {
                Ok(0) => continue,
                Ok(_) => {
                    let revents = poll_fd.revents().unwrap_or(PollFlags::empty());
                    if revents.contains(PollFlags::POLLERR) || revents.contains(PollFlags::POLLPRI) {
                        let mut buf = [0u8; 1];
                        if let Ok(bytes_read) = file.read(&mut buf) {
                            if bytes_read == 1 {
                                let pressed = buf[0] != b'0';
                                log::debug!("fod_press_status changed: {}", 
                                           if pressed { "pressed" } else { "released" });

                                let is_screen_off_enabled = 
                                    android_properties::get_bool("persist.vendor.sys.fp.screen_off", true);
                                
                                if !is_screen_off_enabled && 
                                   DisplayController::get_brightness().unwrap_or(0) == 0 {
                                    log::info!("UDFPS: Touch ignored. Screen-Off disabled.");
                                    continue;
                                }

                                touch.set_finger_down(pressed);
                                display.set_hbm(pressed);

                                if !pressed {
                                    // Clean up on release - will be handled by handler
                                }
                            }
                        }
                    }
                }
                Err(e) => {
                    if e != nix::Error::EINTR {
                        log::error!("Poll failed: {}", e);
                        break;
                    }
                }
            }
        }

        log::info!("FOD press monitor thread stopped (Rust)");
    }
}

/// Monitor for display events
pub struct DisplayMonitor {
    display_controller: Arc<DisplayController>,
    device: *mut fingerprint_device_t,
    running: AtomicBool,
    thread: Option<thread::JoinHandle<()>>,
}

impl DisplayMonitor {
    pub fn new(display: Arc<DisplayController>) -> Self {
        Self {
            display_controller: display,
            device: std::ptr::null_mut(),
            running: AtomicBool::new(false),
            thread: None,
        }
    }

    pub fn set_device(&mut self, device: *mut fingerprint_device_t) {
        self.device = device;
    }

    pub fn start(&mut self) {
        if self.running.load(Ordering::SeqCst) {
            return;
        }

        self.running.store(true, Ordering::SeqCst);
        let display = Arc::clone(&self.display_controller);
        let running = Arc::new(self.running.clone());

        self.thread = Some(thread::spawn(move || {
            Self::monitor_thread(display, running);
        }));
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::SeqCst);
        if let Some(thread) = self.thread.as_ref() {
            let _ = thread.join();
        }
    }

    fn monitor_thread(display: Arc<DisplayController>, running: Arc<AtomicBool>) {
        log::info!("Display event monitor thread started (Rust)");

        if !display.register_events() {
            log::error!("Failed to register for display events");
            return;
        }

        let path = std::path::Path::new(DISP_FEATURE_PATH);
        let mut file = match File::options()
            .read(true)
            .write(true)
            .open(path)
        {
            Ok(f) => f,
            Err(e) => {
                log::error!("Failed to open {}: {}", DISP_FEATURE_PATH, e);
                return;
            }
        };

        let raw_fd = file.as_raw_fd();
        let mut poll_fd = PollFd::new(raw_fd, PollFlags::POLLIN);

        while running.load(Ordering::SeqCst) {
            match poll(&mut [&mut poll_fd], 1000) {
                Ok(0) => continue,
                Ok(_) => {
                    let revents = poll_fd.revents().unwrap_or(PollFlags::empty());
                    if revents.contains(PollFlags::POLLIN) {
                        let mut event_data = [0u8; 1024];
                        if let Ok(size) = file.read(&mut event_data) {
                            if size >= std::mem::size_of::<disp_event_resp>() {
                                let resp = unsafe {
                                    std::ptr::read(event_data.as_ptr() as *const disp_event_resp)
                                };
                                
                                if resp.base.type_ == MI_DISP_EVENT_FOD {
                                    let value = resp.data[0];
                                    log::debug!("Display event data: 0x{:x}", value);

                                    let local_hbm_ui_ready = (value & LOCAL_HBM_UI_READY) != 0;
                                    log::debug!("Local HBM UI Ready: {}", local_hbm_ui_ready);
                                }
                            }
                        }
                    }
                }
                Err(e) => {
                    if e != nix::Error::EINTR {
                        log::error!("Display poll failed: {}", e);
                        break;
                    }
                }
            }
        }

        log::info!("Display event monitor thread stopped (Rust)");
    }
}

/// Monitor for screen state changes
pub struct ScreenStateMonitor {
    touch_controller: Arc<TouchController>,
    display_controller: Arc<DisplayController>,
    device: *mut fingerprint_device_t,
    running: AtomicBool,
    thread: Option<thread::JoinHandle<()>>,
}

impl ScreenStateMonitor {
    pub fn new(touch: Arc<TouchController>, display: Arc<DisplayController>) -> Self {
        Self {
            touch_controller: touch,
            display_controller: display,
            device: std::ptr::null_mut(),
            running: AtomicBool::new(false),
            thread: None,
        }
    }

    pub fn set_device(&mut self, device: *mut fingerprint_device_t) {
        self.device = device;
    }

    pub fn start(&mut self) {
        if self.running.load(Ordering::SeqCst) {
            return;
        }

        self.running.store(true, Ordering::SeqCst);
        let touch = Arc::clone(&self.touch_controller);
        let display = Arc::clone(&self.display_controller);
        let running = Arc::new(self.running.clone());

        self.thread = Some(thread::spawn(move || {
            Self::monitor_thread(touch, display, running);
        }));
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::SeqCst);
        if let Some(thread) = self.thread.as_ref() {
            let _ = thread.join();
        }
    }

    fn monitor_thread(
        touch: Arc<TouchController>,
        _display: Arc<DisplayController>,
        running: Arc<AtomicBool>,
    ) {
        let mut last_state = -1;

        while running.load(Ordering::SeqCst) {
            if let Some(brightness) = DisplayController::get_brightness() {
                let current_state = if brightness == 0 { 0 } else { 1 };
                let is_screen_off_enabled = 
                    android_properties::get_bool("persist.vendor.sys.fp.screen_off", true);

                if current_state != last_state {
                    if current_state == 0 && is_screen_off_enabled {
                        touch.set_fod_status(FOD_STATUS_ON);
                    } else if current_state == 1 {
                        touch.set_fod_status(FOD_STATUS_OFF);
                    }
                    last_state = current_state;
                }
            }
            thread::sleep(Duration::from_millis(200));
        }
    }
}
