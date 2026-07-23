/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

use core::ffi::c_int;

// Include generated bindings if using bindgen
#[cfg(feature = "bindgen")]
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

// Manual bindings (fallback)
#[cfg(not(feature = "bindgen"))]
mod manual {
    use super::*;

    // Constants from mi_disp.h
    pub const MI_DISP_PRIMARY: i32 = 0;
    pub const MI_DISP_IOCTL_SET_LOCAL_HBM: u32 = 0xC00C4601;
    pub const MI_DISP_IOCTL_REGISTER_EVENT: u32 = 0xC00C4602;
    pub const MI_DISP_EVENT_FOD: i32 = 1;
    pub const LOCAL_HBM_UI_READY: i32 = 1;

    // HBM values
    pub const LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT: i32 = 1;
    pub const LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP: i32 = 0;

    // FOD constants
    pub const COMMAND_NIT: i32 = 10;
    pub const PARAM_NIT_FOD: i32 = 1;
    pub const PARAM_NIT_NONE: i32 = 0;

    pub const COMMAND_FOD_PRESS_STATUS: i32 = 1;
    pub const PARAM_FOD_PRESSED: i32 = 1;
    pub const PARAM_FOD_RELEASED: i32 = 0;

    pub const FOD_STATUS_OFF: i32 = 0;
    pub const FOD_STATUS_ON: i32 = 1;

    // Touch constants
    pub const TOUCH_DEV_PATH: &[u8; 18] = b"/dev/xiaomi-touch\0";
    pub const DISP_FEATURE_PATH: &[u8; 25] = b"/dev/mi_display/disp_feature\0";
    pub const FOD_PRESS_STATUS_PATH: &[u8; 39] = b"/sys/class/touch/touch_dev/fod_press_status\0";
    pub const BRIGHTNESS_PATH: &[u8; 38] = b"/sys/class/backlight/panel0-backlight/brightness\0";

    pub const MAX_BUF_SIZE: usize = 256;

    // IOCTL magic
    const TOUCH_MAGIC: u8 = b'T';
    const SET_CUR_VALUE: u8 = 0;
    const GET_CUR_VALUE: u8 = 1;

    // _IO(TOUCH_MAGIC, SET_CUR_VALUE)
    pub const TOUCH_IOC_SET_CUR_VALUE: u32 = 
        ((TOUCH_MAGIC as u32) << 8) | (SET_CUR_VALUE as u32);
    pub const TOUCH_IOC_GET_CUR_VALUE: u32 = 
        ((TOUCH_MAGIC as u32) << 8) | (GET_CUR_VALUE as u32);

    // Mode types from xiaomi_touch.h
    #[repr(i32)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum ModeType {
        TouchGameMode = 0,
        TouchActiveMode = 1,
        TouchUpThreshold = 2,
        TouchTolerance = 3,
        TouchAimSensitivity = 4,
        TouchTapStability = 5,
        TouchExpertMode = 6,
        TouchEdgeFilter = 7,
        TouchPanelOrientation = 8,
        TouchReportRate = 9,
        TouchFodEnable = 10,
        TouchAodEnable = 11,
        TouchResistRf = 12,
        TouchIdleTime = 13,
        TouchDoubletapMode = 14,
        TouchGripMode = 15,
        TouchFodIconEnable = 16,
        TouchNonuiMode = 17,
        TouchDebugLevel = 18,
        TouchPowerStatus = 19,
        TouchModeNum = 20,
        ThpLockScanMode = 1000,
        ThpFodDownupCtl = 1001,
        ThpSelfCapScan = 1002,
        ThpReportPointSwitch = 1003,
        ThpHalInitReady = 1004,
    }

    // Display structures
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct disp_base {
        pub flag: u32,
        pub disp_id: i32,
    }

    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct disp_event_req {
        pub base: disp_base,
        pub type_: i32,
    }

    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct disp_event_resp {
        pub base: disp_base,
        pub type_: i32,
        pub data: [i32; 10],
    }

    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct disp_local_hbm_req {
        pub base: disp_base,
        pub local_hbm_value: i32,
    }

    // Fingerprint device interface
    #[repr(C)]
    pub struct fingerprint_device_t {
        pub ext_cmd: Option<unsafe extern "C" fn(*mut fingerprint_device_t, i32, i32) -> i32>,
        _private: [u8; 0],
    }

    // Acquired info from AIDL
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

    // Thread periods
    pub const POLL_TIMEOUT_MS: i32 = 1000;
    pub const CLEANUP_DELAY_MS: u64 = 500;
    pub const FORCE_CLEANUP_DELAY_MS: u64 = 3000;
}

#[cfg(not(feature = "bindgen"))]
pub use manual::*;

#[cfg(feature = "bindgen")]
pub use crate::bindings::*;
