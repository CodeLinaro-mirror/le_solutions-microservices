// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// DEPRECATED — PredictiveAIOrchestrator.cpp
//
// This file is no longer compiled (removed from CMakeLists.txt in Phase 4).
// PredictiveAIOrchestrator has been superseded by PredictiveModelPool.
//
// QaiForge::infer() now routes through PredictiveModelPool, which provides:
//   - Multiple models loaded simultaneously (one PredictiveModelRuntime each)
//   - LRU eviction when max_active_models is exceeded
//   - Idle-timeout eviction for unused models
//   - Per-model serialization + cross-model concurrency
//   - Owned backend instances (not singletons) via BackendFactory::createPredictiveBackend()
//
// See qai-forge/docs/phase4-predictive-scheduler.md for the full design.
// ─────────────────────────────────────────────────────────────────────────────
