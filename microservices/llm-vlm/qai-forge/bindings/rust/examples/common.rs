// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Shared helper for the Rust binding examples.
//
// Not published — included directly via `include!("common.rs")` by each
// example binary, since Cargo examples cannot easily share library code
// without a separate crate.

#[allow(dead_code)]
/// Fallback default used by every example when neither a CLI argument nor
/// the `QAI_FORGE_MODEL` environment variable is set.
pub const DEFAULT_MODEL: &str = "Qwen3-1.7B";

/// Resolves the model id to use for an example run.
///
/// Resolution order:
///   1. `std::env::args().nth(1)`   e.g. `cargo run --example 01_simple_chat -- Qwen3-1.7B`
///   2. `QAI_FORGE_MODEL` env var
///   3. The given fallback default
pub fn resolve_model(fallback: &str) -> String {
    if let Some(arg) = std::env::args().nth(1) {
        if !arg.is_empty() {
            return arg;
        }
    }
    if let Ok(env) = std::env::var("QAI_FORGE_MODEL") {
        if !env.is_empty() {
            return env;
        }
    }
    fallback.to_string()
}
