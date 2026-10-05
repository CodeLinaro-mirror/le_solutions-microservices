// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! 02_streaming_chat: streaming chat completion.
//!
//! Run:
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example 02_streaming_chat -- [model_id]
//! ```

include!("common.rs");

use qai_forge::{generate_stream, ChatMessage, ChatRequest};

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
            content: "Tell me a short joke.".to_string(),
        }],
        ..Default::default()
    };

    print!("Response: ");
    match generate_stream(&req) {
        Ok(chunks) => {
            for chunk in &chunks {
                if let Some(delta) = &chunk.content_delta {
                    print!("{}", delta);
                }
            }
            if let Some(last) = chunks.last() {
                if let Some(reason) = &last.finish_reason {
                    println!("\n[finish_reason: {}]", reason);
                }
            }
        }
        Err(e) => eprintln!("Stream error: {}", e),
    }

    qai_forge::shutdown(false);
}
