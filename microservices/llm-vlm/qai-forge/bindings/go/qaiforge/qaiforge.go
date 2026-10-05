// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Package qaiforge provides Go bindings for the qai-forge Layer 2 C API.
//
// Calls QaiForge directly in-process via cgo — no HTTP/gRPC/D-Bus
// server needed.
//
// Build requirements:
//   - libqai_forge.so must be in the linker search path, or set
//     CGO_LDFLAGS="-L/path/to/lib -lqai_forge"
//   - The qai_forge C API header must be in the C include path, or set
//     CGO_CFLAGS="-I/path/to/include"
//
// Example:
//
//	resp, err := qaiforge.Chat(qaiforge.ChatRequest{
//	    Model:    "Qwen3-1.7B",
//	    Messages: []qaiforge.ChatMessage{{Role: "user", Content: "Hello"}},
//	})
//	if err != nil {
//	    log.Fatal(err)
//	}
//	fmt.Println(resp.Content)
package qaiforge

/*
#cgo LDFLAGS: -lqai_forge
#include "qai_forge/c_api/qai_forge_c_api.h"
#include <stdlib.h>

// streamCallbackBridge is a C function that can be used as a C callback.
// It receives a Go channel handle via user_data and sends the chunk JSON to it.
// Defined in bridge.go (as a //export function).
extern void streamCallbackBridge(const char* chunk_json, void* user_data);
*/
import "C"
import (
	"encoding/json"
	"fmt"
	"runtime/cgo"
	"unsafe"
)

// ── Public types ───────────────────────────────────────────────────────────────

// ChatMessage is a single message in the conversation.
type ChatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

// ChatRequest is the input to Chat and ChatStream.
type ChatRequest struct {
	Model     string        `json:"model"`
	Messages  []ChatMessage `json:"messages"`
	MaxTokens int           `json:"max_completion_tokens,omitempty"`
	// Temperature for sampling (0.0–2.0). Zero value uses the model default.
	Temperature float32 `json:"temperature,omitempty"`
	TopP        float32 `json:"top_p,omitempty"`
	TopK        int     `json:"top_k,omitempty"`
	// User scopes private Genie runtime memory. Messages must contain the
	// complete conversation history on every request.
	User string `json:"user,omitempty"`
}

// ChatResponse is the full non-streaming response from Chat.
type ChatResponse struct {
	ID               string `json:"id"`
	Model            string `json:"model"`
	Content          string `json:"content"`
	ReasoningContent string `json:"reasoning_content,omitempty"`
	FinishReason     string `json:"finish_reason"`
	PromptTokens     int    `json:"prompt_tokens"`
	CompletionTokens int    `json:"completion_tokens"`
	TotalTokens      int    `json:"total_tokens"`
}

// StreamChunk is a single streaming token delta from ChatStream.
type StreamChunk struct {
	ID             string `json:"id"`
	Model          string `json:"model"`
	ContentDelta   string `json:"content_delta,omitempty"`
	ReasoningDelta string `json:"reasoning_delta,omitempty"`
	FinishReason   string `json:"finish_reason,omitempty"`
	Role           string `json:"role,omitempty"`
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

// Start starts the internal scheduler (loads backend plugins).
// Must be called once before any Generate/GenerateStream call. Safe to call
// multiple times — subsequent calls are no-ops.
func Start() {
	C.qai_forge_start()
}

// ScanModels scans the models directory (GENAI_MODELS_DIR env var, or the
// built-in default) and (re)populates the model registry used by
// Generate()/GenerateStream() for model-id validation and backend/runtime
// routing.
//
// MUST be called at least once after Start() and before any
// Generate/GenerateStream call — otherwise every request fails with a 404
// "Model not found" error, since the registry starts empty. Safe to call
// again later to hot-reload if models are added/removed on disk.
func ScanModels() {
	C.qai_forge_scan_models()
}

// Shutdown shuts down the internal scheduler.
// force=false drains in-flight requests gracefully; force=true aborts them
// immediately.
func Shutdown(force bool) {
	if force {
		C.qai_forge_shutdown(1)
	} else {
		C.qai_forge_shutdown(0)
	}
}

// Cancel cancels an in-flight generative request by response_id.
// Returns true if the request was found and cancelled.
func Cancel(responseID string) bool {
	if responseID == "" {
		return false
	}
	cID := C.CString(responseID)
	defer C.free(unsafe.Pointer(cID))
	return C.qai_forge_cancel(cID) != 0
}

// ── Public API ─────────────────────────────────────────────────────────────────

// Version returns the library version string.
func Version() string {
	return C.GoString(C.qai_forge_version())
}

// SupportedRuntimes returns a JSON string describing which generative and
// predictive runtimes this build of libqai_forge.so supports, e.g.
// `{"generative":["genie","litert_lm"],"predictive":["qnn","snpe"]}`.
func SupportedRuntimes() string {
	return C.GoString(C.qai_forge_supported_runtimes())
}

// ReleaseConversation releases private runtime memory associated with user.
func ReleaseConversation(user string) bool {
	if user == "" {
		return false
	}
	cUser := C.CString(user)
	defer C.free(unsafe.Pointer(cUser))
	return C.qai_forge_release_conversation(cUser) != 0
}

// Generate runs a blocking chat completion.
// It blocks until generation is complete and returns the full response.
func Generate(req ChatRequest) (*ChatResponse, error) {
	reqJSON, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("QaiForge: marshal request: %w", err)
	}

	cReq := C.CString(string(reqJSON))
	defer C.free(unsafe.Pointer(cReq))

	var cResp *C.char
	var cErr *C.char

	rc := C.qai_forge_generate(cReq, &cResp, &cErr)
	if rc != 0 {
		errMsg := "unknown error"
		if cErr != nil {
			errMsg = C.GoString(cErr)
			C.qai_forge_free_string(cErr)
		}
		return nil, fmt.Errorf("QaiForge: Chat failed [%d]: %s", rc, errMsg)
	}
	defer C.qai_forge_free_string(cResp)

	var resp ChatResponse
	if err := json.Unmarshal([]byte(C.GoString(cResp)), &resp); err != nil {
		return nil, fmt.Errorf("QaiForge: unmarshal response: %w", err)
	}
	return &resp, nil
}

// GenerateStream runs a streaming chat completion.
//
// Returns two channels:
//   - chunks: receives StreamChunk values as tokens arrive; closed when done.
//   - errs:   receives at most one error; closed after chunks is closed.
//
// The C API is synchronous, so inference runs in a goroutine. Tokens are
// forwarded to the chunks channel via a cgo.Handle.
//
// Example:
//
//	chunks, errs := qaiforge.GenerateStream(req)
//	for chunk := range chunks {
//	    fmt.Print(chunk.ContentDelta)
//	}
//	if err := <-errs; err != nil {
//	    log.Fatal(err)
//	}
func GenerateStream(req ChatRequest) (<-chan StreamChunk, <-chan error) {
	chunks := make(chan StreamChunk, 64)
	errs := make(chan error, 1)

	go func() {
		defer close(chunks)
		defer close(errs)

		reqJSON, err := json.Marshal(req)
		if err != nil {
			errs <- fmt.Errorf("QaiForge: marshal request: %w", err)
			return
		}

		cReq := C.CString(string(reqJSON))
		defer C.free(unsafe.Pointer(cReq))

		// Pass the channel to the C callback via a cgo.Handle.
		// The handle is deleted after the streaming call returns.
		handle := cgo.NewHandle(chunks)
		defer handle.Delete()

		var cErr *C.char
		rc := C.qai_forge_generate_stream(
			cReq,
			C.qai_forge_stream_cb_t(C.streamCallbackBridge),
			unsafe.Pointer(&handle),
			&cErr,
		)
		if rc != 0 {
			errMsg := "unknown error"
			if cErr != nil {
				errMsg = C.GoString(cErr)
				C.qai_forge_free_string(cErr)
			}
			errs <- fmt.Errorf("QaiForge: GenerateStream failed [%d]: %s", rc, errMsg)
		}
	}()

	return chunks, errs
}

// ── Deprecated aliases (kept for one release cycle) ─────────────────────────
//
// These names were used before Generate()/GenerateStream() were introduced
// to match the QaiForge facade and C API naming (qai_forge_generate /
// qai_forge_generate_stream). Prefer the new names.

// Chat is a deprecated alias for Generate.
func Chat(req ChatRequest) (*ChatResponse, error) {
	return Generate(req)
}

// ChatStream is a deprecated alias for GenerateStream.
func ChatStream(req ChatRequest) (<-chan StreamChunk, <-chan error) {
	return GenerateStream(req)
}
