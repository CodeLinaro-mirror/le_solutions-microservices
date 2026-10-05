// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! 05_sampling_params: custom sampling parameters.
//!
//! Run:
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example 05_sampling_params -- [model_id]
//! ```

include!("common.rs");

use qai_forge::{generate, ChatMessage, ChatRequest};

fn main() {
    let model = resolve_model(DEFAULT_MODEL);
    println!(
        "Model: {} (override with argv[1] or QAI_FORGE_MODEL env var)\n",
        model
    );

    qai_forge::start();
    qai_forge::scan_models();

    let req = ChatRequest {
        model,
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "Write a haiku about the ocean.".to_string(),
        }],
        max_completion_tokens: Some(64),
        temperature: Some(0.7),
        top_p: Some(0.9),
        top_k: Some(50),
        ..Default::default()
    };

    match generate(&req) {
        Ok(resp) => println!("{}", resp.content),
        Err(e) => eprintln!("Error: {}", e),
    }

    qai_forge::shutdown(false);
}
