// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// 02_streaming_chat: streaming chat completion.
//
// Build:
//
//	CGO_CFLAGS="-I/path/to/qai-forge/include" \
//	CGO_LDFLAGS="-L/path/to/lib -lqai_forge" \
//	go run main.go [model_id]
package main

import (
	"fmt"
	"log"
	"os"

	qaiforge "qaiforge/qaiforge"
)

func main() {
	model := resolveModel(os.Args[1:], "Qwen3-1.7B")
	fmt.Printf("Model: %s (override with argv[1] or QAI_FORGE_MODEL env var)\n\n", model)

	qaiforge.Start()
	defer qaiforge.Shutdown(false)
	qaiforge.ScanModels()

	fmt.Print("Response: ")
	chunks, errs := qaiforge.GenerateStream(qaiforge.ChatRequest{
		Model:    model,
		Messages: []qaiforge.ChatMessage{{Role: "user", Content: "Tell me a short joke."}},
	})
	for chunk := range chunks {
		if chunk.ContentDelta != "" {
			fmt.Print(chunk.ContentDelta)
		}
		if chunk.FinishReason != "" {
			fmt.Printf("\n[finish_reason: %s]\n", chunk.FinishReason)
		}
	}
	if err := <-errs; err != nil {
		log.Fatalf("Stream error: %v", err)
	}
}

func resolveModel(args []string, fallback string) string {
	if len(args) > 0 && args[0] != "" {
		return args[0]
	}
	if env := os.Getenv("QAI_FORGE_MODEL"); env != "" {
		return env
	}
	return fallback
}
