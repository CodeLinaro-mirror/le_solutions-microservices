// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// 05_sampling_params: custom sampling parameters.
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
		Model:       model,
		Messages:    []qaiforge.ChatMessage{{Role: "user", Content: "Write a haiku about the ocean."}},
		MaxTokens:   64,
		Temperature: 0.7,
		TopP:        0.9,
		TopK:        50,
	})
	if err != nil {
		log.Fatalf("Generate error: %v", err)
	}
	fmt.Println(resp.Content)
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
