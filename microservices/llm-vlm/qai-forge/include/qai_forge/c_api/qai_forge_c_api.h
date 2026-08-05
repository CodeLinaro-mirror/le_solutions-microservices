// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge_c_api.h — Flat C API for Layer 2 (ChatOrchestrator)
//
// Exposes ChatOrchestrator::handleBlocking() and handleStreaming() via a
// stable C ABI. All request/response data is exchanged as JSON strings.
//
// This header is the universal FFI boundary for Python (ctypes), Go (cgo),
// Rust (extern "C"), and plain C applications.
//
// Thread safety: All functions are thread-safe. The underlying
// ConcurrencyMiddleware DSP lock serializes concurrent inference calls.
//
// See: docs/language-bindings-proposal.md
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// ── Blocking completion ───────────────────────────────────────────────────────
//
// Runs inference synchronously. Blocks until generation is complete.
//
// Parameters:
//   request_json      — JSON string (CreateChatCompletionRequest format, see below)
//   response_json_out — On success: heap-allocated JSON string. Caller MUST free
//                       with qai_forge_free_string(). NULL on error.
//   error_out         — On error: heap-allocated error message. Caller MUST free
//                       with qai_forge_free_string(). NULL on success.
//
// Returns: 0 on success, non-zero on error.
//
// Request JSON format:
//   {
//     "model":                 "Qwen3-1.7B",          // required
//     "messages":              [{"role":"user","content":"Hello"}],  // required
//     "max_completion_tokens": 1024,                  // optional
//     "temperature":           1.0,                   // optional
//     "top_p":                 1.0,                   // optional
//     "top_k":                 40,                    // optional
//     "user":                  "session-id"           // optional (multi-turn)
//   }
//
// Response JSON format:
//   {
//     "id":                "...",
//     "model":             "Qwen3-1.7B",
//     "content":           "Hello! How can I help?",
//     "reasoning_content": "...",   // only for thinking models (Qwen3, DeepSeek-R1)
//     "finish_reason":     "stop",  // "stop" | "length" | "tool_calls"
//     "prompt_tokens":     12,
//     "completion_tokens": 8,
//     "total_tokens":      20
//   }
int qai_forge_chat_blocking(const char*  request_json,
                           char**       response_json_out,
                           char**       error_out);

// ── Streaming completion ──────────────────────────────────────────────────────
//
// Runs inference and calls the callback for each token as it is generated.
// Blocks until generation is complete (all callbacks have been called).
//
// Parameters:
//   request_json — JSON string (same format as qai_forge_chat_blocking)
//   callback     — Called once per token. chunk_json is a JSON string:
//                  {
//                    "id":              "...",
//                    "model":           "Qwen3-1.7B",
//                    "content_delta":   "Hello",   // token text (intermediate)
//                    "reasoning_delta": "...",      // thinking models only
//                    "finish_reason":   "stop",     // non-empty on FINAL call only
//                    "role":            "assistant" // first call only
//                  }
//                  IMPORTANT: chunk_json is valid ONLY for the duration of the
//                  callback. Do NOT free it — it is stack-allocated by the C API.
//                  Copy it if you need it to outlive the callback.
//   user_data    — Opaque pointer passed through to every callback invocation.
//                  May be NULL.
//   error_out    — On error: heap-allocated error message. Caller MUST free.
//                  NULL on success.
//
// Returns: 0 on success, non-zero on error.
typedef void (*qai_forge_stream_cb_t)(const char* chunk_json, void* user_data);

int qai_forge_chat_streaming(const char*          request_json,
                            qai_forge_stream_cb_t  callback,
                            void*                user_data,
                            char**               error_out);

// ── Memory management ─────────────────────────────────────────────────────────
//
// Free a string allocated by qai_forge_chat_blocking() or any error_out pointer.
// Safe to call with NULL (no-op).
void qai_forge_free_string(char* str);

// ── Version ───────────────────────────────────────────────────────────────────
//
// Returns a static version string (e.g. "1.0.0"). Do NOT free this pointer.
const char* qai_forge_version(void);

#ifdef __cplusplus
}
#endif
