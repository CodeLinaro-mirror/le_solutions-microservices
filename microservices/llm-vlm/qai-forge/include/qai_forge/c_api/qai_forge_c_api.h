// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge_c_api.h — Flat C API for qai-forge inference
//
// Exposes QaiForge::generate() and QaiForge::generateStream() via a stable
// C ABI. All request/response data is exchanged as JSON strings.
//
// This header is the universal FFI boundary for Python (ctypes), Go (cgo),
// Rust (extern "C"), and plain C applications.
//
// Thread safety: All functions are thread-safe. The internal scheduler
// serializes per-model execution while allowing concurrent requests.
//
// See: docs/qaiforge-api-design.md
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// ── Streaming callback type ───────────────────────────────────────────────────
//
// Called once per token during streaming inference.
// chunk_json is valid ONLY for the duration of the callback — it is
// stack-allocated. Copy it if you need it to outlive the callback.
typedef void (*qai_forge_stream_cb_t)(const char* chunk_json, void* user_data);

// ─────────────────────────────────────────────────────────────────────────────
// Generative AI — LLM / VLM
// ─────────────────────────────────────────────────────────────────────────────

// ── qai_forge_generate — blocking generative inference ───────────────────────
//
// Runs inference synchronously. Blocks until generation is complete.
//
// Parameters:
//   request_json      — JSON string (CreateChatCompletionRequest format):
//                       {
//                         "model":                 "Qwen3-1.7B",
//                         "messages":              [{"role":"user","content":"Hello"}],
//                         "max_completion_tokens": 1024,   // optional
//                         "temperature":           1.0,    // optional
//                         "top_p":                 1.0,    // optional
//                         "top_k":                 40,     // optional
//                         "user":                  "sid"   // optional (multi-turn)
//                       }
//   response_json_out — On success: heap-allocated JSON string. Caller MUST free
//                       with qai_forge_free_string(). NULL on error.
//                       {
//                         "id":                "...",
//                         "model":             "Qwen3-1.7B",
//                         "content":           "Hello! How can I help?",
//                         "reasoning_content": "...",   // thinking models only
//                         "finish_reason":     "stop",
//                         "prompt_tokens":     12,
//                         "completion_tokens": 8,
//                         "total_tokens":      20
//                       }
//   error_out         — On error: heap-allocated error message. Caller MUST free.
//                       NULL on success.
//
// Returns: 0 on success, HTTP status code on error, -1 on internal error.
int qai_forge_generate(const char*  request_json,
                       char**       response_json_out,
                       char**       error_out);

// ── qai_forge_generate_stream — streaming generative inference ────────────────
//
// Runs inference and calls the callback for each token as it is generated.
// Blocks until generation is complete (all callbacks have been called).
//
// Parameters:
//   request_json — JSON string (same format as qai_forge_generate)
//   callback     — Called once per token. chunk_json format:
//                  {
//                    "id":              "...",
//                    "model":           "Qwen3-1.7B",
//                    "content_delta":   "Hello",    // token text (intermediate)
//                    "reasoning_delta": "...",       // thinking models only
//                    "finish_reason":   "stop",      // non-empty on FINAL call only
//                    "role":            "assistant"  // first call only
//                  }
//   user_data    — Opaque pointer passed through to every callback. May be NULL.
//   error_out    — On error: heap-allocated error message. Caller MUST free.
//
// Returns: 0 on success, HTTP status code on error, -1 on internal error.
int qai_forge_generate_stream(const char*           request_json,
                               qai_forge_stream_cb_t callback,
                               void*                 user_data,
                               char**                error_out);

// ─────────────────────────────────────────────────────────────────────────────
// Predictive AI — classification / detection / segmentation
// ─────────────────────────────────────────────────────────────────────────────

// ── qai_forge_infer — tensor inference ───────────────────────────────────────
//
// Runs a predictive AI tensor inference request synchronously.
// Routes to QNN, SNPE, or LiteRT based on the model's runtime field.
//
// Parameters:
//   request_json      — JSON string (TensorInferenceRequest format):
//                       {
//                         "model":   "mobilenet-v3",
//                         "inputs":  [{"name":"input","datatype":"FP32",
//                                      "shape":[1,224,224,3],"data":[...]}],
//                         "outputs": ["output"]   // optional output filter
//                       }
//   response_json_out — On success: heap-allocated JSON string. Caller MUST free.
//   error_out         — On error: heap-allocated error message. Caller MUST free.
//
// Returns: 0 on success, HTTP status code on error, -1 on internal error.
int qai_forge_infer(const char*  request_json,
                    char**       response_json_out,
                    char**       error_out);

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

// Start the internal scheduler. Must be called once at startup.
// Safe to call multiple times — subsequent calls are no-ops.
void qai_forge_start(void);

// Shut down the internal scheduler.
// force=0: drain in-flight requests gracefully.
// force=1: abort in-flight requests immediately.
void qai_forge_shutdown(int force);

// Cancel an in-flight generative request by response_id.
// Returns 1 if cancelled, 0 if not found.
int qai_forge_cancel(const char* response_id);

// ─────────────────────────────────────────────────────────────────────────────
// Memory management
// ─────────────────────────────────────────────────────────────────────────────

// Free a string allocated by any qai_forge_* function.
// Safe to call with NULL (no-op).
void qai_forge_free_string(char* str);

// Returns a static version string (e.g. "1.0.0"). Do NOT free this pointer.
const char* qai_forge_version(void);

// ─────────────────────────────────────────────────────────────────────────────
// Deprecated aliases (kept for one release cycle)
//
// These names were used before the QaiForge unified facade was introduced.
// Use qai_forge_generate() and qai_forge_generate_stream() instead.
// ─────────────────────────────────────────────────────────────────────────────
static inline int qai_forge_chat_blocking(const char* req, char** resp, char** err) {
    return qai_forge_generate(req, resp, err);
}
static inline int qai_forge_chat_streaming(const char* req,
                                            qai_forge_stream_cb_t cb,
                                            void* ud, char** err) {
    return qai_forge_generate_stream(req, cb, ud, err);
}

#ifdef __cplusplus
}
#endif
