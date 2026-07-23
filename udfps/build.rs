// build.rs - Generates Rust bindings from C headers

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=bindings/wrapper.h");
    println!("cargo:rerun-if-changed=include/xiaomi_touch.h");
    
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    
    // Generate bindings from C headers
    let bindings = bindgen::Builder::default()
        .header("bindings/wrapper.h")
        .clang_arg("-Iinclude")
        .clang_arg("-Iexternal/libcxx/include")
        .clang_arg("-Isystem/core/libcutils/include")
        .clang_arg("-Ihardware/interfaces/biometrics/fingerprint/4.0/default")
        .allowlist_type("ModeType")
        .allowlist_type("disp_base")
        .allowlist_type("disp_event_req")
        .allowlist_type("disp_event_resp")
        .allowlist_type("disp_local_hbm_req")
        .allowlist_type("fingerprint_device_t")
        .allowlist_var("TOUCH_.*")
        .allowlist_var("THP_.*")
        .allowlist_var("MI_DISP_.*")
        .allowlist_var("LHBM_.*")
        .allowlist_var("COMMAND_.*")
        .allowlist_var("PARAM_.*")
        .allowlist_var("FOD_STATUS_.*")
        .allowlist_var("MAX_BUF_SIZE")
        .allowlist_var("SET_CUR_VALUE")
        .allowlist_var("GET_CUR_VALUE")
        .allowlist_var("TOUCH_MAGIC")
        .blocklist_type(".*__u8")
        .blocklist_type(".*__u16")
        .blocklist_type(".*__u32")
        .blocklist_type(".*__u64")
        .blocklist_type(".*__s8")
        .blocklist_type(".*__s16")
        .blocklist_type(".*__s32")
        .blocklist_type(".*__s64")
        .no_layout_tests()
        .generate()
        .expect("Unable to generate bindings");
    
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
    
    println!("cargo:rustc-link-search=native=/system/lib64");
    println!("cargo:rustc-link-lib=log");
    println!("cargo:rustc-link-lib=cutils");
    println!("cargo:rustc-link-lib=base");
}
