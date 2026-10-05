// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! 04_multi_turn: multi-turn conversation via the `user` session scope.
//!
//! Run:
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example 04_multi_turn -- [model_id]
//! ```

include!("common.rs");

use qai_forge::{generate, release_conversation, ChatMessage, ChatRequest};

fn main() {
    let model = resolve_model(DEFAULT_MODEL);
    println!(
        "Model: {} (override with argv[1] or QAI_FORGE_MODEL env var)\n",
        model
    );

    qai_forge::start();
    qai_forge::scan_models();

    let session = "rust-example-session-001".to_string();

    let first_message = ChatMessage {
        role: "user".to_string(),
        content: "My name is Carol and I love Rust.".to_string(),
    };
    let req1 = ChatRequest {
        model: model.clone(),
        messages: vec![first_message.clone()],
        user: Some(session.clone()),
        ..Default::default()
    };

    match generate(&req1) {
        Ok(r1) => {
            println!("Turn 1: {}", r1.content);
            let req2 = ChatRequest {
                model,
                messages: vec![
                    first_message,
                    ChatMessage {
                        role: "assistant".to_string(),
                        content: r1.content,
                    },
                    ChatMessage {
                        role: "user".to_string(),
                        content: "What is my name and what do I love?".to_string(),
                    },
                ],
                user: Some(session.clone()),
                ..Default::default()
            };
            match generate(&req2) {
                Ok(r2) => println!("Turn 2: {}", r2.content),
                Err(e) => eprintln!("Turn 2 error: {}", e),
            }
        }
        Err(e) => eprintln!("Turn 1 error: {}", e),
    }

    let _ = release_conversation(&session);
    qai_forge::shutdown(false);
}
