// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// DEPRECATED — PredictiveAIOrchestrator
//
// This file is retained for reference only. PredictiveAIOrchestrator has been
// superseded by PredictiveModelPool (Phase 4 — Predictive Scheduler).
//
// Migration table:
//   Before (Phase 3)                          After (Phase 4)
//   ─────────────────────────────────────     ──────────────────────────────────
//   PredictiveAIOrchestrator::getInstance()   scheduler::PredictiveModelPool
//   .handleInfer(request)                     .infer(request)
//   QNNBackend::getInstance()                 BackendFactory::createPredictiveBackend("qnn")
//   SNPEBackend::getInstance()                BackendFactory::createPredictiveBackend("snpe")
//   LiteRTBackend::getInstance()              BackendFactory::createPredictiveBackend("litert")
//
// QaiForge::infer() now routes through PredictiveModelPool internally.
// The public API (QaiForge::infer) is unchanged.
//
// See qai-forge/docs/phase4-predictive-scheduler.md for the full design.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

// This header is intentionally empty. Do not include it in new code.
// Use qai_forge/scheduler/PredictiveModelPool.h instead.
