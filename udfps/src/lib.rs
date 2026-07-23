/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

// Module declarations
mod bindings;
mod display;
mod handler;
mod monitors;
mod touch;

// Re-exports
pub use handler::XiaomiSm6225UdfpsHandler;

use core::ffi::c_void;
use core::ptr;

// Initialize Android logging
#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn RustInit() {
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("UdfpsHandler.rs")
            .with_max_level(log::LevelFilter::Info)
    );
}

// C ABI factory functions
#[no_mangle]
pub extern "C" fn create_udfps_handler() -> *mut c_void {
    log::info!("Creating UDFPS handler (Rust implementation)");
    let handler = XiaomiSm6225UdfpsHandler::new();
    let boxed: Box<dyn handler::UdfpsHandler> = Box::new(handler);
    Box::into_raw(boxed) as *mut c_void
}

#[no_mangle]
pub extern "C" fn destroy_udfps_handler(handler: *mut c_void) {
    if !handler.is_null() {
        log::info!("Destroying UDFPS handler (Rust implementation)");
        unsafe { drop(Box::from_raw(handler as *mut dyn handler::UdfpsHandler)) };
    }
}

// C ABI factory struct
#[repr(C)]
pub struct UdfpsHandlerFactory {
    pub create: unsafe extern "C" fn() -> *mut c_void,
    pub destroy: unsafe extern "C" fn(*mut c_void),
}

#[no_mangle]
pub static UDFPS_HANDLER_FACTORY: UdfpsHandlerFactory = UdfpsHandlerFactory {
    create: create_udfps_handler,
    destroy: destroy_udfps_handler,
};

// Ensure the struct layout matches C++ expectations
#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn udfps_handler_factory() -> *const UdfpsHandlerFactory {
    &UDFPS_HANDLER_FACTORY
}
