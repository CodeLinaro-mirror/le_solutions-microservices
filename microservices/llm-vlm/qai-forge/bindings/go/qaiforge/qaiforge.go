// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Package qaiforge provides Go bindings for the qai-forge Layer 2 C API.
//
// Calls ChatOrchestrator directly in-process via cgo — no HTTP/gRPC/D-Bus
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
	// User is an optional session ID for multi-turn conversations.
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

// ── Public API ─────────────────────────────────────────────────────────────────

// Version returns the library version string.
func Version() string {
	return C.GoString(C.qai_forge_version())
}

// Chat runs a blocking chat completion.
// It blocks until generation is complete and returns the full response.
func Chat(req ChatRequest) (*ChatResponse, error) {
	reqJSON, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("QaiForge: marshal request: %w", err)
	}

	cReq := C.CString(string(reqJSON))
	defer C.free(unsafe.Pointer(cReq))

	var cResp *C.char
	var cErr *C.char

	rc := C.qai_forge_chat_blocking(cReq, &cResp, &cErr)
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

// ChatStream runs a streaming chat completion.
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
//	chunks, errs := qaiforge.ChatStream(req)
//	for chunk := range chunks {
//	    fmt.Print(chunk.ContentDelta)
//	}
//	if err := <-errs; err != nil {
//	    log.Fatal(err)
//	}
func ChatStream(req ChatRequest) (<-chan StreamChunk, <-chan error) {
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
		rc := C.qai_forge_chat_streaming(
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
			errs <- fmt.Errorf("QaiForge: ChatStream failed [%d]: %s", rc, errMsg)
		}
	}()

	return chunks, errs
}
