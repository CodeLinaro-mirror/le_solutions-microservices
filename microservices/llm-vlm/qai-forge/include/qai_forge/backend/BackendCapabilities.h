// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// BackendCapabilities — Capability contract for IGenerativeBackend implementations
//
// Each IGenerativeBackend declares its capabilities via this struct.
// GenerativeOrchestrator reads it at startup to configure the middleware pipeline.
// No orchestrator code branches on backend type strings — it only reads these flags.
//
// Design rationale (docs/genai-backend-decoupling.md):
//   Decouples GenIE-specific assumptions (DSP lock, KV reset, bypass_think_filter)
//   from ChatOrchestratorImpl so the orchestrator works with any generative backend.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * How the backend manages its context window across conversation turns.
 *
 * RESET_KV:       Backend has a stateful KV cache. After context compaction
 *                 (summarization), the KV cache must be reset and the compacted
 *                 context re-fed from scratch.
 *                 Used by: GenIE (GenieDialog_reset), LiteRT LM (ResetContext).
 *
 * FULL_RECOMPUTE: Backend recomputes the full context from scratch each turn.
 *                 No KV reset is needed after summarization — the next call
 *                 naturally starts fresh.
 *                 Used by: ONNX Runtime (past_key_values rebuilt each call).
 *
 * SLIDING_WINDOW: Future — drop oldest tokens, keep recent context window.
 */
enum class ContextStrategy {
    RESET_KV,
    FULL_RECOMPUTE,
    SLIDING_WINDOW,
};

/**
 * How the backend handles concurrent inference requests.
 *
 * EXCLUSIVE:  Only one inference at a time (DSP/NPU hardware lock).
 *             Used by: GenIE (Snapdragon DSP), LiteRT LM (GPU lock).
 *
 * BOUNDED:    Up to max_concurrent inferences simultaneously.
 *             Used by: ONNX Runtime on CPU/GPU.
 *
 * UNLIMITED:  No concurrency limit (CPU-only, future).
 */
enum class ConcurrencyModel {
    EXCLUSIVE,
    BOUNDED,
    UNLIMITED,
};

/**
 * BackendCapabilities — declared by each IGenerativeBackend implementation.
 *
 * The GenerativeOrchestrator reads this struct at startup to build the
 * appropriate middleware pipeline. No orchestrator code branches on backend
 * type strings — it only reads these capability flags.
 */
struct BackendCapabilities {
    // ── Context management ────────────────────────────────────────────────────
    ContextStrategy context_strategy     = ContextStrategy::RESET_KV;
    int             context_window       = 4096;   // max tokens this backend supports
    float           compaction_threshold = 0.70f;  // trigger summarization at 70%

    // ── Concurrency ───────────────────────────────────────────────────────────
    ConcurrencyModel concurrency_model   = ConcurrencyModel::EXCLUSIVE;
    int              max_concurrent      = 1;

    // ── Reasoning ─────────────────────────────────────────────────────────────
    // backend_filters_think_tokens:
    //   true  → GenIE SDK can silently filter <think> tokens internally.
    //            Controlled by bypass_think_filter flag in the EXECUTE command.
    //            When false (bypass_think_filter=false), GenIE strips think tokens.
    //            When true (bypass_think_filter=true), raw tokens reach Layer 2.
    //   false → Backend always outputs raw tokens. ReasoningRouter in Layer 2
    //            handles routing for all backends where this is false.
    //            (LiteRT LM, ONNX Runtime)
    bool backend_filters_think_tokens    = false;

    // ── KV cache ──────────────────────────────────────────────────────────────
    // supports_kv_save_restore:
    //   true  → GenIE: GenieDialog_save() / GenieDialog_restore() available.
    //            Used for precise reasoning budget enforcement: save KV state
    //            before <think>, restore if budget exceeded.
    //   false → LiteRT LM, ONNX Runtime: no checkpoint API available.
    //            Budget enforced by token counting only (approximate).
    bool supports_kv_save_restore        = false;
};
