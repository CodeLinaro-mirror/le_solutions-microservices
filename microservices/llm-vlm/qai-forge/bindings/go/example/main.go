// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Go bindings usage examples.
//
// Build:
//
//	CGO_CFLAGS="-I/path/to/qai-forge/include" \
//	CGO_LDFLAGS="-L/path/to/lib -lqai_forge" \
//	go run main.go
package main

import (
	"fmt"
	"log"

	"QaiForge"
)

const model = "Qwen3-1.7B"

func main() {
	fmt.Printf("qai-forge version: %s\n", qaiforge.Version())

	// ── Example 1: Blocking completion ────────────────────────────────────────
	fmt.Println("\n=== Blocking completion ===")
	resp, err := qaiforge.Chat(qaiforge.ChatRequest{
		Model:    model,
		Messages: []qaiforge.ChatMessage{{Role: "user", Content: "What is 2 + 2?"}},
	})
	if err != nil {
		log.Fatalf("Chat error: %v", err)
	}
	fmt.Printf("Answer: %s\n", resp.Content)
	fmt.Printf("Tokens: %d prompt / %d completion\n", resp.PromptTokens, resp.CompletionTokens)

	// ── Example 2: Streaming completion ───────────────────────────────────────
	fmt.Println("\n=== Streaming completion ===")
	fmt.Print("Response: ")
	chunks, errs := qaiforge.ChatStream(qaiforge.ChatRequest{
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

	// ── Example 3: Reasoning model ────────────────────────────────────────────
	fmt.Println("\n=== Reasoning model (thinking tokens) ===")
	resp, err = qaiforge.Chat(qaiforge.ChatRequest{
		Model: model,
		Messages: []qaiforge.ChatMessage{
			{Role: "user", Content: "What is the square root of 144? Think step by step."},
		},
		MaxTokens: 512,
	})
	if err != nil {
		log.Fatalf("Chat error: %v", err)
	}
	if resp.ReasoningContent != "" {
		fmt.Printf("<think>\n%s\n</think>\n", resp.ReasoningContent)
	}
	fmt.Printf("Answer: %s\n", resp.Content)

	// ── Example 4: Multi-turn conversation ────────────────────────────────────
	fmt.Println("\n=== Multi-turn conversation ===")
	const session = "go-example-session-001"

	r1, err := qaiforge.Chat(qaiforge.ChatRequest{
		Model:    model,
		Messages: []qaiforge.ChatMessage{{Role: "user", Content: "My name is Bob and I love Go."}},
		User:     session,
	})
	if err != nil {
		log.Fatalf("Turn 1 error: %v", err)
	}
	fmt.Printf("Turn 1: %s\n", r1.Content)

	r2, err := qaiforge.Chat(qaiforge.ChatRequest{
		Model:    model,
		Messages: []qaiforge.ChatMessage{{Role: "user", Content: "What is my name and what do I love?"}},
		User:     session,
	})
	if err != nil {
		log.Fatalf("Turn 2 error: %v", err)
	}
	fmt.Printf("Turn 2: %s\n", r2.Content)

	// ── Example 5: Custom sampling parameters ─────────────────────────────────
	fmt.Println("\n=== Custom sampling parameters ===")
	resp, err = qaiforge.Chat(qaiforge.ChatRequest{
		Model:       model,
		Messages:    []qaiforge.ChatMessage{{Role: "user", Content: "Write a haiku about the ocean."}},
		MaxTokens:   64,
		Temperature: 0.7,
		TopP:        0.9,
		TopK:        50,
	})
	if err != nil {
		log.Fatalf("Chat error: %v", err)
	}
	fmt.Println(resp.Content)
}
