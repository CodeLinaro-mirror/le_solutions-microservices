// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// 01_simple_chat: blocking chat completion.
//
// Build:
//
//	CGO_CFLAGS="-I/path/to/qai-forge/include" \
//	CGO_LDFLAGS="-L/path/to/lib -lqai_forge" \
//	go run main.go [model_id]
//
// Model selection (checked in order):
//  1. os.Args[1]
//  2. QAI_FORGE_MODEL env var
//  3. Fallback default ("Qwen3-1.7B")
package main

import (
	"fmt"
	"log"
	"os"

	qaiforge "qaiforge/qaiforge"
)

func main() {
	model := resolveModel(os.Args[1:], "Qwen3-1.7B")

	fmt.Printf("qai-forge version: %s\n", qaiforge.Version())
	fmt.Printf("Model: %s (override with argv[1] or QAI_FORGE_MODEL env var)\n\n", model)

	qaiforge.Start()
	defer qaiforge.Shutdown(false)
	qaiforge.ScanModels()

	resp, err := qaiforge.Generate(qaiforge.ChatRequest{
		Model:    model,
		Messages: []qaiforge.ChatMessage{{Role: "user", Content: "What is 2 + 2?"}},
	})
	if err != nil {
		log.Fatalf("Generate error: %v", err)
	}
	fmt.Printf("Answer: %s\n", resp.Content)
	fmt.Printf("Tokens: %d prompt / %d completion\n", resp.PromptTokens, resp.CompletionTokens)
}

// resolveModel resolves the model id: args[0] > QAI_FORGE_MODEL env var > fallback.
func resolveModel(args []string, fallback string) string {
	if len(args) > 0 && args[0] != "" {
		return args[0]
	}
	if env := os.Getenv("QAI_FORGE_MODEL"); env != "" {
		return env
	}
	return fallback
}
