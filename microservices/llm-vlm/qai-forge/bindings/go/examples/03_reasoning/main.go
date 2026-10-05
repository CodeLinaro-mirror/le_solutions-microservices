// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// 03_reasoning: reasoning model (thinking tokens).
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

	resp, err := qaiforge.Generate(qaiforge.ChatRequest{
		Model: model,
		Messages: []qaiforge.ChatMessage{
			{Role: "user", Content: "What is the square root of 144? Think step by step."},
		},
		MaxTokens: 512,
	})
	if err != nil {
		log.Fatalf("Generate error: %v", err)
	}
	if resp.ReasoningContent != "" {
		fmt.Printf("<think>\n%s\n</think>\n", resp.ReasoningContent)
	}
	fmt.Printf("Answer: %s\n", resp.Content)
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
