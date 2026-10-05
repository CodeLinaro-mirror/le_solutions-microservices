// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// 04_multi_turn: multi-turn conversation via the User session scope.
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

const session = "go-example-session-001"

func main() {
	model := resolveModel(os.Args[1:], "Qwen3-1.7B")
	fmt.Printf("Model: %s (override with argv[1] or QAI_FORGE_MODEL env var)\n\n", model)

	qaiforge.Start()
	defer qaiforge.Shutdown(false)
	defer qaiforge.ReleaseConversation(session)
	qaiforge.ScanModels()

	messages := []qaiforge.ChatMessage{
		{Role: "user", Content: "My name is Bob and I love Go."},
	}

	r1, err := qaiforge.Generate(qaiforge.ChatRequest{
		Model:    model,
		Messages: messages,
		User:     session,
	})
	if err != nil {
		log.Fatalf("Turn 1 error: %v", err)
	}
	fmt.Printf("Turn 1: %s\n", r1.Content)

	messages = append(messages,
		qaiforge.ChatMessage{Role: "assistant", Content: r1.Content},
		qaiforge.ChatMessage{Role: "user", Content: "What is my name and what do I love?"},
	)
	r2, err := qaiforge.Generate(qaiforge.ChatRequest{
		Model:    model,
		Messages: messages,
		User:     session,
	})
	if err != nil {
		log.Fatalf("Turn 2 error: %v", err)
	}
	fmt.Printf("Turn 2: %s\n", r2.Content)
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
