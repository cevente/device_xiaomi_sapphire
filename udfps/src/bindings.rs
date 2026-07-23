/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

// Include generated bindings
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

// Manual constants that might not be in headers
pub const COMMAND_NIT: i32 = 10;
pub const PARAM_NIT_FOD: i32 = 1;
pub const PARAM_NIT_NONE: i32 = 0;

pub const COMMAND_FOD_PRESS_STATUS: i32 = 1;
pub const PARAM_FOD_PRESSED: i32 = 1;
pub const PARAM_FOD_RELEASED: i32 = 0;

pub const FOD_STATUS_OFF: i32 = 0;
pub const FOD_STATUS_ON: i32 = 1;

pub const MI_DISP_PRIMARY: i32 = 0;
pub const MI_DISP_EVENT_FOD: i32 = 1;
pub const LOCAL_HBM_UI_READY: i32 = 1;

pub const TOUCH_DEV_PATH: &str = "/dev/xiaomi-touch";
pub const DISP_FEATURE_PATH: &str = "/dev/mi_display/disp_feature";
pub const FOD_PRESS_STATUS_PATH: &str = "/sys/class/touch/touch_dev/fod_press_status";
pub const BRIGHTNESS_PATH: &str = "/sys/class/backlight/panel0-backlight/brightness";

pub const POLL_TIMEOUT_MS: i32 = 1000;
pub const CLEANUP_DELAY_MS: u64 = 500;
pub const FORCE_CLEANUP_DELAY_MS: u64 = 3000;

// AcquiredInfo from AIDL
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AcquiredInfo {
    Good = 0,
    Partial = 1,
    Insufficient = 2,
    SensorDirty = 3,
    TooSlow = 4,
    TooFast = 5,
    Immobile = 6,
    Vendor = 7,
}

impl TryFrom<i32> for AcquiredInfo {
    type Error = ();

    fn try_from(value: i32) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(AcquiredInfo::Good),
            1 => Ok(AcquiredInfo::Partial),
            2 => Ok(AcquiredInfo::Insufficient),
            3 => Ok(AcquiredInfo::SensorDirty),
            4 => Ok(AcquiredInfo::TooSlow),
            5 => Ok(AcquiredInfo::TooFast),
            6 => Ok(AcquiredInfo::Immobile),
            7 => Ok(AcquiredInfo::Vendor),
            _ => Err(()),
        }
    }
}
