// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//! # qai-forge — Rust bindings for qai-forge Layer 2
//!
//! Calls `ChatOrchestrator` directly in-process via the C FFI —
//! no HTTP/gRPC/D-Bus server needed.
//!
//! ## Build
//!
//! ```bash
//! QAI_FORGE_LIB_DIR=/path/to/lib cargo build
//! ```
//!
//! ## Usage
//!
//! ```rust,no_run
//! use qai_forge::{chat, ChatRequest, ChatMessage};
//!
//! let req = ChatRequest {
//!     model: "Qwen3-1.7B".to_string(),
//!     messages: vec![ChatMessage {
//!         role: "user".to_string(),
//!         content: "Hello!".to_string(),
//!     }],
//!     ..Default::default()
//! };
//!
//! let resp = chat(&req).unwrap();
//! println!("{}", resp.content);
//! ```

use std::ffi::{CStr, CString, NulError};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::{Arc, Mutex};

use serde::{Deserialize, Serialize};

// ── Raw FFI declarations ───────────────────────────────────────────────────────
extern "C" {
    fn qai_forge_chat_blocking(
        request_json: *const c_char,
        response_json_out: *mut *mut c_char,
        error_out: *mut *mut c_char,
    ) -> c_int;

    fn qai_forge_chat_streaming(
        request_json: *const c_char,
        callback: unsafe extern "C" fn(*const c_char, *mut c_void),
        user_data: *mut c_void,
        error_out: *mut *mut c_char,
    ) -> c_int;

    fn qai_forge_free_string(s: *mut c_char);
    fn qai_forge_version() -> *const c_char;
}

// ── Public types ───────────────────────────────────────────────────────────────

/// A single message in the conversation.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String,
    pub content: String,
}

/// Input to [`chat`] and [`chat_stream`].
#[derive(Debug, Clone, Serialize)]
pub struct ChatRequest {
    pub model: String,
    pub messages: Vec<ChatMessage>,
    /// Maximum number of completion tokens.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_completion_tokens: Option<i32>,
    /// Sampling temperature (0.0–2.0).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub top_p: Option<f32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub top_k: Option<i32>,
    /// Optional session ID for multi-turn conversations.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub user: Option<String>,
}

impl Default for ChatRequest {
    fn default() -> Self {
        Self {
            model: String::new(),
            messages: Vec::new(),
            max_completion_tokens: None,
            temperature: None,
            top_p: None,
            top_k: None,
            user: None,
        }
    }
}

/// Full non-streaming response from [`chat`].
#[derive(Debug, Deserialize)]
pub struct ChatResponse {
    pub id: String,
    pub model: String,
    pub content: String,
    /// Populated for reasoning models (Qwen3, DeepSeek-R1).
    pub reasoning_content: Option<String>,
    pub finish_reason: String,
    pub prompt_tokens: i32,
    pub completion_tokens: i32,
    pub total_tokens: i32,
}

/// Single streaming token delta from [`chat_stream`].
#[derive(Debug, Deserialize)]
pub struct StreamChunk {
    pub id: String,
    pub model: String,
    /// Token text (intermediate chunks).
    pub content_delta: Option<String>,
    /// Thinking content (reasoning models only).
    pub reasoning_delta: Option<String>,
    /// Non-empty on the final chunk: `"stop"`, `"length"`, `"tool_calls"`.
    pub finish_reason: Option<String>,
    /// `"assistant"` on the first chunk only.
    pub role: Option<String>,
}

/// Error type for all qai-forge operations.
#[derive(Debug, thiserror::Error)]
pub enum QaiForgeError {
    #[error("C API error [{code}]: {message}")]
    CApi { code: i32, message: String },

    #[error("JSON serialization error: {0}")]
    Json(#[from] serde_json::Error),

    #[error("Null byte in request string: {0}")]
    NulByte(#[from] NulError),

    #[error("Null pointer returned by C API")]
    NullPointer,
}

// ── Public API ─────────────────────────────────────────────────────────────────

/// Returns the library version string (e.g. `"1.0.0"`).
pub fn version() -> &'static str {
    unsafe { CStr::from_ptr(qai_forge_version()) }
        .to_str()
        .unwrap_or("unknown")
}

/// Blocking chat completion.
///
/// Blocks until generation is complete and returns the full response.
///
/// # Errors
///
/// Returns [`QaiForgeError`] if the C API returns a non-zero error code,
/// if JSON serialization fails, or if the response cannot be deserialized.
pub fn chat(req: &ChatRequest) -> Result<ChatResponse, QaiForgeError> {
    let req_json = CString::new(serde_json::to_string(req)?)?;

    let mut resp_ptr: *mut c_char = std::ptr::null_mut();
    let mut err_ptr: *mut c_char = std::ptr::null_mut();

    let rc = unsafe {
        qai_forge_chat_blocking(req_json.as_ptr(), &mut resp_ptr, &mut err_ptr)
    };

    if rc != 0 {
        let message = if err_ptr.is_null() {
            "unknown error".to_string()
        } else {
            let s = unsafe { CStr::from_ptr(err_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { qai_forge_free_string(err_ptr) };
            s
        };
        return Err(QaiForgeError::CApi { code: rc, message });
    }

    if resp_ptr.is_null() {
        return Err(QaiForgeError::NullPointer);
    }

    let resp_str = unsafe { CStr::from_ptr(resp_ptr) }
        .to_string_lossy()
        .into_owned();
    unsafe { qai_forge_free_string(resp_ptr) };

    Ok(serde_json::from_str(&resp_str)?)
}

/// Streaming chat completion.
///
/// Returns a `Vec<StreamChunk>` collected synchronously.
/// The C API blocks until generation is complete; all chunks are collected
/// via the C callback and returned as an iterator.
///
/// For async streaming, wrap this in a `tokio::task::spawn_blocking` call.
///
/// # Errors
///
/// Returns [`QaiForgeError`] if the C API returns a non-zero error code.
pub fn chat_stream(req: &ChatRequest) -> Result<Vec<StreamChunk>, QaiForgeError> {
    let req_json = CString::new(serde_json::to_string(req)?)?;

    // Collect chunks via the C callback into a shared Vec
    let chunks: Arc<Mutex<Vec<StreamChunk>>> = Arc::new(Mutex::new(Vec::new()));
    let chunks_ptr = Arc::as_ptr(&chunks) as *mut c_void;

    // C callback — called from the C++ thread for each token
    unsafe extern "C" fn on_chunk(chunk_json: *const c_char, user_data: *mut c_void) {
        if chunk_json.is_null() || user_data.is_null() {
            return;
        }
        let chunks = &*(user_data as *const Mutex<Vec<StreamChunk>>);
        if let Ok(s) = CStr::from_ptr(chunk_json).to_str() {
            if let Ok(chunk) = serde_json::from_str::<StreamChunk>(s) {
                if let Ok(mut guard) = chunks.lock() {
                    guard.push(chunk);
                }
            }
        }
    }

    let mut err_ptr: *mut c_char = std::ptr::null_mut();

    let rc = unsafe {
        qai_forge_chat_streaming(req_json.as_ptr(), on_chunk, chunks_ptr, &mut err_ptr)
    };

    if rc != 0 {
        let message = if err_ptr.is_null() {
            "unknown error".to_string()
        } else {
            let s = unsafe { CStr::from_ptr(err_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { qai_forge_free_string(err_ptr) };
            s
        };
        return Err(QaiForgeError::CApi { code: rc, message });
    }

    // qai_forge_chat_streaming blocks until all callbacks are done
    let result = Arc::try_unwrap(chunks)
        .map_err(|_| QaiForgeError::CApi {
            code: -1,
            message: "Arc still has multiple owners after streaming".to_string(),
        })?
        .into_inner()
        .map_err(|_| QaiForgeError::CApi {
            code: -1,
            message: "Mutex poisoned".to_string(),
        })?;

    Ok(result)
}
