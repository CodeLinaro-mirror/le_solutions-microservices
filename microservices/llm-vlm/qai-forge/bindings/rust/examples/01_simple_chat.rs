// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! 01_simple_chat: blocking chat completion.
//!
//! Run:
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example 01_simple_chat -- [model_id]
//! ```

include!("common.rs");

use qai_forge::{generate, ChatMessage, ChatRequest};

fn main() {
    let model = resolve_model(DEFAULT_MODEL);
    println!("qai-forge version: {}", qai_forge::version());
    println!(
        "Model: {} (override with argv[1] or QAI_FORGE_MODEL env var)\n",
        model
    );

    qai_forge::start();
    qai_forge::scan_models();

    let req = ChatRequest {
        model: model.clone(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "What is 2 + 2?".to_string(),
        }],
        ..Default::default()
    };

    match generate(&req) {
        Ok(resp) => {
            println!("Answer: {}", resp.content);
            println!(
                "Tokens: {} prompt / {} completion",
                resp.prompt_tokens, resp.completion_tokens
            );
        }
        Err(e) => eprintln!("Error: {}", e),
    }

    qai_forge::shutdown(false);
}
