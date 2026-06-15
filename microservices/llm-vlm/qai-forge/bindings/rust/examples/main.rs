// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! Rust bindings usage examples.
//!
//! Build and run:
//!
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo run --example chat
//! ```

use qai_forge::{chat, chat_stream, ChatMessage, ChatRequest};

const MODEL: &str = "Qwen3-1.7B";

fn main() {
    println!("qai-forge version: {}", qai_forge::version());

    // ── Example 1: Blocking completion ────────────────────────────────────────
    println!("\n=== Blocking completion ===");
    let req = ChatRequest {
        model: MODEL.to_string(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "What is 2 + 2?".to_string(),
        }],
        ..Default::default()
    };

    match chat(&req) {
        Ok(resp) => {
            println!("Answer: {}", resp.content);
            println!(
                "Tokens: {} prompt / {} completion",
                resp.prompt_tokens, resp.completion_tokens
            );
        }
        Err(e) => eprintln!("Error: {}", e),
    }

    // ── Example 2: Streaming completion ───────────────────────────────────────
    println!("\n=== Streaming completion ===");
    let req = ChatRequest {
        model: MODEL.to_string(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "Tell me a short joke.".to_string(),
        }],
        ..Default::default()
    };

    print!("Response: ");
    match chat_stream(&req) {
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

    // ── Example 3: Reasoning model ────────────────────────────────────────────
    println!("\n=== Reasoning model (thinking tokens) ===");
    let req = ChatRequest {
        model: MODEL.to_string(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "What is the square root of 144? Think step by step.".to_string(),
        }],
        max_completion_tokens: Some(512),
        ..Default::default()
    };

    match chat(&req) {
        Ok(resp) => {
            if let Some(thinking) = &resp.reasoning_content {
                println!("<think>\n{}\n</think>", thinking);
            }
            println!("Answer: {}", resp.content);
        }
        Err(e) => eprintln!("Error: {}", e),
    }

    // ── Example 4: Multi-turn conversation ────────────────────────────────────
    println!("\n=== Multi-turn conversation ===");
    let session = "rust-example-session-001".to_string();

    let req1 = ChatRequest {
        model: MODEL.to_string(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "My name is Carol and I love Rust.".to_string(),
        }],
        user: Some(session.clone()),
        ..Default::default()
    };

    match chat(&req1) {
        Ok(r) => println!("Turn 1: {}", r.content),
        Err(e) => eprintln!("Turn 1 error: {}", e),
    }

    let req2 = ChatRequest {
        model: MODEL.to_string(),
        messages: vec![ChatMessage {
            role: "user".to_string(),
            content: "What is my name and what do I love?".to_string(),
        }],
        user: Some(session),
        ..Default::default()
    };

    match chat(&req2) {
        Ok(r) => println!("Turn 2: {}", r.content),
        Err(e) => eprintln!("Turn 2 error: {}", e),
    }

    // ── Example 5: Custom sampling parameters ─────────────────────────────────
    println!("\n=== Custom sampling parameters ===");
    let req = ChatRequest {
        model: MODEL.to_string(),
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

    match chat(&req) {
        Ok(resp) => println!("{}", resp.content),
        Err(e) => eprintln!("Error: {}", e),
    }
}
