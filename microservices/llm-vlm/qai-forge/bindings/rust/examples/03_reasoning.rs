// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! 03_reasoning: reasoning model (thinking tokens).
//!
//! Run:
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example 03_reasoning -- [model_id]
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
            content: "What is the square root of 144? Think step by step.".to_string(),
        }],
        max_completion_tokens: Some(512),
        ..Default::default()
    };

    match generate(&req) {
        Ok(resp) => {
            if let Some(thinking) = &resp.reasoning_content {
                println!("<think>\n{}\n</think>", thinking);
            }
            println!("Answer: {}", resp.content);
        }
        Err(e) => eprintln!("Error: {}", e),
    }

    qai_forge::shutdown(false);
}
