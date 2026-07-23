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

use nix::ioctl_none_bad;
use nix::libc::{ioctl, O_RDWR};

use crate::bindings::*;

/// Touch controller for Xiaomi devices
pub struct TouchController {
    fd: Mutex<Option<OwnedFd>>,
    last_fod_status: Mutex<i32>,
}

impl TouchController {
    pub fn new() -> Self {
        Self {
            fd: Mutex::new(None),
            last_fod_status: Mutex::new(FOD_STATUS_OFF),
        }
    }

    /// Initialize touch device
    pub fn init(&self) {
        let path = Path::new(TOUCH_DEV_PATH);
        let file = match File::options()
            .read(true)
            .write(true)
            .custom_flags(O_RDWR)
            .open(path)
        {
            Ok(f) => f,
            Err(e) => {
                log::error!("Failed to open touch device: {}", e);
                return;
            }
        };

        let fd = file.into();
        *self.fd.lock().unwrap() = Some(fd);
        log::info!("Touch device opened successfully");
    }

    /// Close touch device
    pub fn close(&self) {
        *self.fd.lock().unwrap() = None;
        log::info!("Touch device closed");
    }

    /// Set FOD status
    pub fn set_fod_status(&self, value: i32) {
        let mut last_status = self.last_fod_status.lock().unwrap();
        if *last_status == value {
            return; // No change needed
        }

        if let Some(fd) = self.fd.lock().unwrap().as_ref() {
            let raw_fd = fd.as_raw_fd();
            let mut buf = [MI_DISP_PRIMARY, ModeType::TouchFodEnable as i32, value];
            let ptr = buf.as_mut_ptr() as *mut libc::c_void;

            unsafe {
                if ioctl(raw_fd, TOUCH_IOC_SET_CUR_VALUE, ptr) < 0 {
                    log::error!("Failed to set FOD status: {}", std::io::Error::last_os_error());
                } else {
                    *last_status = value;
                    log::debug!("Set FOD status to {}", value);
                }
            }
        }
    }

    /// Set finger down state
    pub fn set_finger_down(&self, pressed: bool) {
        if let Some(fd) = self.fd.lock().unwrap().as_ref() {
            let raw_fd = fd.as_raw_fd();
            let mut buf = [MI_DISP_PRIMARY, ModeType::ThpFodDownupCtl as i32, pressed as i32];
            let ptr = buf.as_mut_ptr() as *mut libc::c_void;

            unsafe {
                if ioctl(raw_fd, TOUCH_IOC_SET_CUR_VALUE, ptr) < 0 {
                    log::error!("Failed to set finger down: {}", std::io::Error::last_os_error());
                } else {
                    log::debug!("Set finger down to {}", pressed);
                }
            }
        }
    }

    /// Reset touch state
    pub fn reset_state(&self) {
        self.set_finger_down(false);
        self.set_fod_status(FOD_STATUS_OFF);
        log::debug!("Touch state reset");
    }

    /// Read FOD press status
    pub fn read_fod_press_status(&self) -> bool {
        let path = Path::new(FOD_PRESS_STATUS_PATH);
        if let Ok(mut file) = File::open(path) {
            let mut buf = [0u8; 1];
            if let Ok(bytes_read) = file.read(&mut buf) {
                if bytes_read == 1 {
                    return buf[0] != b'0';
                }
            }
        }
        false
    }
}

impl Drop for TouchController {
    fn drop(&mut self) {
        self.close();
    }
}
