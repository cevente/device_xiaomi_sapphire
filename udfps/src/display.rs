/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

use std::fs::File;
use std::io::Read;
use std::os::fd::{AsRawFd, OwnedFd};
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;
use std::sync::Mutex;

use nix::libc::{ioctl, O_RDWR};

use crate::bindings::*;

/// Display controller for Xiaomi devices
pub struct DisplayController {
    fd: Mutex<Option<OwnedFd>>,
    hbm_enabled: Mutex<bool>,
}

impl DisplayController {
    pub fn new() -> Self {
        Self {
            fd: Mutex::new(None),
            hbm_enabled: Mutex::new(false),
        }
    }

    pub fn init(&self) {
        let path = Path::new(DISP_FEATURE_PATH);
        let file = match File::options()
            .read(true)
            .write(true)
            .custom_flags(O_RDWR)
            .open(path)
        {
            Ok(f) => f,
            Err(e) => {
                log::error!("Failed to open display device: {}", e);
                return;
            }
        };

        let fd = file.into();
        *self.fd.lock().unwrap() = Some(fd);
        log::info!("Display device opened successfully");
    }

    pub fn close(&self) {
        *self.fd.lock().unwrap() = None;
        log::info!("Display device closed");
    }

    pub fn set_hbm(&self, enabled: bool) {
        let mut hbm_state = self.hbm_enabled.lock().unwrap();
        if *hbm_state == enabled {
            return;
        }

        if let Some(fd) = self.fd.lock().unwrap().as_ref() {
            let raw_fd = fd.as_raw_fd();
            let mut req = disp_local_hbm_req {
                base: disp_base {
                    flag: 0,
                    disp_id: MI_DISP_PRIMARY,
                },
                local_hbm_value: if enabled {
                    LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                } else {
                    LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP
                },
            };
            let ptr = &mut req as *mut disp_local_hbm_req as *mut libc::c_void;

            unsafe {
                if ioctl(raw_fd, MI_DISP_IOCTL_SET_LOCAL_HBM, ptr) < 0 {
                    log::error!("Failed to set HBM: {}", std::io::Error::last_os_error());
                } else {
                    *hbm_state = enabled;
                    log::debug!("Set HBM to {}", enabled);
                }
            }
        }
    }

    pub fn force_hbm_off(&self) {
        self.set_hbm(false);
        log::debug!("Forced HBM off");
    }

    pub fn register_events(&self) -> bool {
        if let Some(fd) = self.fd.lock().unwrap().as_ref() {
            let raw_fd = fd.as_raw_fd();
            let mut req = disp_event_req {
                base: disp_base {
                    flag: 0,
                    disp_id: MI_DISP_PRIMARY,
                },
                type_: MI_DISP_EVENT_FOD,
            };
            let ptr = &mut req as *mut disp_event_req as *mut libc::c_void;

            unsafe {
                if ioctl(raw_fd, MI_DISP_IOCTL_REGISTER_EVENT, ptr) < 0 {
                    log::error!("Failed to register for display events: {}", 
                               std::io::Error::last_os_error());
                    return false;
                }
            }
            log::info!("Registered for display events");
            return true;
        }
        false
    }

    pub fn get_brightness() -> Option<i32> {
        let path = Path::new(BRIGHTNESS_PATH);
        if let Ok(mut file) = File::open(path) {
            let mut buf = [0u8; 12];
            if let Ok(len) = file.read(&mut buf) {
                if len > 0 {
                    let s = String::from_utf8_lossy(&buf[..len]);
                    return s.trim().parse().ok();
                }
            }
        }
        None
    }
}

impl Drop for DisplayController {
    fn drop(&mut self) {
        self.close();
    }
}
