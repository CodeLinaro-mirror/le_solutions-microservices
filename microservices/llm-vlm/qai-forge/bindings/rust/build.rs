// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! build.rs — Links libqai_forge.so for the Rust bindings.
//!
//! Set QAI_FORGE_LIB_DIR to the directory containing libqai_forge.so:
//!   QAI_FORGE_LIB_DIR=/path/to/lib cargo build
//!
//! If QAI_FORGE_LIB_DIR is not set, the linker will search the default paths
//! (LD_LIBRARY_PATH, /usr/lib, /usr/local/lib, etc.).

fn main() {
    // Tell cargo to re-run this script if the env var changes
    println!("cargo:rerun-if-env-changed=QAI_FORGE_LIB_DIR");

    if let Ok(lib_dir) = std::env::var("QAI_FORGE_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", lib_dir);
    }

    println!("cargo:rustc-link-lib=dylib=qai_forge");
}
